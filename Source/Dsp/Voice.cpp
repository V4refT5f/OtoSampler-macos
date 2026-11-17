#include "Voice.h"

namespace oto
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

struct HannTable
{
    float t[1026];
    HannTable() { for (int i = 0; i < 1026; ++i) t[i] = 0.5f - 0.5f * (float) std::cos (2.0 * kPi * i / 1024.0); }
};
const HannTable& hannTable() { static const HannTable h; return h; }

inline float hannAt (float x) noexcept
{
    const auto& t = hannTable().t;
    const float p = x * 1024.f; const int i = (int) p; const float f = p - (float) i;
    return t[i] + (t[i + 1] - t[i]) * f;
}

// 4-point Hermite, zero outside the buffer
inline float readSample (const float* d, int n, double pos) noexcept
{
    const int i = (int) std::floor (pos);
    const float f = (float) (pos - i);
    float y0, y1, y2, y3;
    if (i >= 1 && i + 2 < n) { y0 = d[i - 1]; y1 = d[i]; y2 = d[i + 1]; y3 = d[i + 2]; }
    else
    {
        auto at = [&] (int k) { return (k >= 0 && k < n) ? d[k] : 0.f; };
        y0 = at (i - 1); y1 = at (i); y2 = at (i + 1); y3 = at (i + 2);
    }
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}

inline float crossW2 (double m) noexcept   // 0..1 -> sin^2 (sums to 1 with cos^2)
{
    const double s = std::sin (std::clamp (m, 0.0, 1.0) * kPi * 0.5);
    return (float) (s * s);
}
}

void Voice::start (const SampleData& d, int midiNote, float vel, float glideFromPitch,
                   const VoiceParams& p, std::uint64_t serial) noexcept
{
    note_ = midiNote;
    serial_ = serial;
    velocity = (1.f - p.velSens) + p.velSens * std::clamp (vel, 0.f, 1.f);
    gated = true; active = true; pedalHeld = false; preview = false;
    const int N = d.numSamples();
    // Start never goes beyond the loop start point while Sustain Loop is on
    const float startNorm = std::min (std::clamp (p.startNorm, 0.f, 1.f),
                                      maxStartNorm (d.source->map, N, p.loopOn, p.loopStartNorm, p.loopEndNorm));
    pos = a = (double) startNorm * std::max (0, N - 1);
    glideOffset = (p.glideMs > 0.5f && glideFromPitch == glideFromPitch) ? glideFromPitch - (float) midiNote : 0.f;
    lfoPhase = 0.0; vibTime = 0.0; initDone = false;
    for (auto& g : grains) g.active = false;
    firstGrain = true; sourceDone = false; spawnCountdown = 0.0; loopArmed = false;
    adsr.setSampleRate (sr);
    adsr.setParams (p.attackMs, p.decayMs, p.sustain, p.releaseMs);
    adsr.kill();
    adsr.noteOn();
}

void Voice::spawnGrain (const SampleData& d, const VoiceParams& p, double r, double k, const LoopInfo& L) noexcept
{
    const PitchMap& m = d.source->map;
    const int mi = m.nearest (a);
    const bool voiced = mi >= 0 && m.voiced[(size_t) mi];
    const double Tin = mi >= 0 ? (double) m.periods[(size_t) mi] : (double) m.defaultPeriod;
    const double c1 = voiced ? (double) m.marks[(size_t) mi] : a;

    // unvoiced parts (consonants/breath) are not pitch-shifted -> no buzz
    const double rEff = voiced ? r : 1.0;
    const double g = (p.formantHold ? 1.0 : r) * k;                 // source samples per output sample
    const double Tbase = (p.flatten && voiced) ? (double) m.refPeriod : Tin;   // flatten: constant output period
    const double Tout = std::clamp (Tbase / (k * rEff), 24.0, 16000.0); // output hop (host samples)
    const double half = std::max (Tout, Tin / g);
    const int len = std::max (16, (int) std::ceil (2.0 * half));

    Grain* slot = nullptr;
    for (auto& gr : grains) if (! gr.active) { slot = &gr; break; }
    if (slot != nullptr)
    {
        slot->active = true; slot->c1 = c1; slot->g = g; slot->len = len;
        slot->age = firstGrain ? len / 2 : 0;       // first grain starts at its peak (no fade-in of the attack)
        slot->gain = (float) (Tout / half);
        slot->w2 = 0.f; slot->shift = 0.0;
        slot->wA = slot->wB = 0.f; slot->offA = slot->offB = 0.0;
        if ((p.formantHold || p.flatten) && p.denoise > 0.f && voiced)
        {
            // Denoise: average with the previous / next pitch period (aperiodic noise cancels, harmonics stay)
            const float wn = std::clamp (p.denoise, 0.f, 1.f) * (1.f / 3.f);
            const int n = (int) m.marks.size();
            if (mi > 0 && m.voiced[(size_t) mi - 1])
            {
                const double off = (double) m.marks[(size_t) mi - 1] - c1;
                if (off < -0.5 * Tin && off > -1.6 * Tin) { slot->offA = off; slot->wA = wn; }
            }
            if (mi + 1 < n && m.voiced[(size_t) mi + 1])
            {
                const double off = (double) m.marks[(size_t) mi + 1] - c1;
                if (off > 0.5 * Tin && off < 1.6 * Tin) { slot->offB = off; slot->wB = wn; }
            }
        }
        if (L.active && a >= L.e - L.xf)
        {
            slot->w2 = crossW2 ((a - (L.e - L.xf)) / L.xf);
            slot->shift = L.len;
        }
    }
    if (firstGrain) { spawnCountdown = 0.0; firstGrain = false; }
    else spawnCountdown += Tout;
}

void Voice::render (const SampleData& d, const VoiceParams& p, float bendTarget,
                    float* outL, float* outR, int n) noexcept
{
    if (! active || d.numChannels() == 0) return;
    const int N = d.numSamples();
    if (N < 4) { kill(); return; }

    const float* c0 = d.channels[0].data();
    const float* c1 = d.numChannels() > 1 ? d.channels[1].data() : c0;
    const double srcSr = d.source->sampleRate;
    const double k = srcSr / sr;

    adsr.setParams (p.attackMs, p.decayMs, p.sustain, p.releaseMs);
    const float normGain = p.normalize ? 0.89f / std::max (1e-6f, d.peak) : 1.f;
    const float targetGain = p.gain * normGain * velocity;
    if (! initDone) { gainSm = targetGain; bendCur = bendTarget; initDone = true; }

    // ---- loop points (snapped to pitch marks => whole number of periods) ----
    // The loop stays active while the note is released, so the release tail fades out inside the loop
    // and the part of the sample after the loop is never played.
    LoopInfo L;
    {
        const auto lr = resolveLoop (d.source->map, N, p.loopStartNorm, p.loopEndNorm);
        if (lr.valid)
        {
            L.s = lr.s; L.e = lr.e; L.len = lr.e - lr.s;
            L.xf = std::max (1.0, std::min ((double) p.loopXfadeMs * 0.001 * srcSr, L.len * 0.5));
            L.active = p.loopOn;
        }
    }

    const bool useGrains = p.formantHold || p.speedHold || p.flatten;
    usingGrains = useGrains;
    const float bendCoef = std::exp (-1.f / (0.005f * (float) sr));
    const float gainCoef = std::exp (-1.f / (0.010f * (float) sr));
    const float glideCoef = p.glideMs > 0.5f ? std::exp (-6.9f / (p.glideMs * 0.001f * (float) sr)) : 0.f;
    const double vibInc = (double) p.vibRate / sr;
    const float vibDepth = p.vibDepthCents * 0.01f;
    const double vibDelay = (double) p.vibDelayMs * 0.001 * sr, vibFade = 0.1 * sr;

    for (int i = 0; i < n; ++i)
    {
        bendCur = bendTarget + (bendCur - bendTarget) * bendCoef;
        gainSm  = targetGain + (gainSm - targetGain) * gainCoef;
        glideOffset *= glideCoef;
        if (std::abs (glideOffset) < 1e-4f) glideOffset = 0.f;

        float vib = 0.f;
        if (vibDepth > 0.f)
        {
            const double onset = std::clamp ((vibTime - vibDelay) / vibFade, 0.0, 1.0);
            vib = vibDepth * (float) onset * (float) std::sin (2.0 * kPi * lfoPhase);
            lfoPhase += vibInc; if (lfoPhase >= 1.0) lfoPhase -= std::floor (lfoPhase);
        }
        vibTime += 1.0;

        const float semis = (float) (note_ - p.rootKey) + p.coarseSemis + bendCur + vib + glideOffset;
        const double r = std::exp2 ((double) std::clamp (semis, -72.f, 72.f) / 12.0);

        const float env = adsr.next();
        if (! adsr.isActive()) { active = false; break; }

        float l = 0.f, rr = 0.f;

        if (! useGrains)
        {
            // ---------- plain resampling (varispeed) ----------
            // the loop only engages once the read position has been before the loop end
            // (starting behind the loop end plays straight through, no jump back)
            if (L.active && ! loopArmed && pos < L.e) loopArmed = true;
            const bool lp = L.active && loopArmed;
            if (lp) while (pos >= L.e) pos -= L.len;
            if (pos >= N) { kill(); break; }
            l = readSample (c0, N, pos); rr = readSample (c1, N, pos);
            if (lp && pos >= L.e - L.xf)
            {
                const float w2 = crossW2 ((pos - (L.e - L.xf)) / L.xf);
                const double p2 = pos - L.len;
                l  = l  * (1.f - w2) + readSample (c0, N, p2) * w2;
                rr = rr * (1.f - w2) + readSample (c1, N, p2) * w2;
            }
            pos += r * k;
        }
        else
        {
            // ---------- grain (PSOLA) engine: speed and/or formant preserved ----------
            if (L.active && ! loopArmed && a < L.e) loopArmed = true;
            const bool lp = L.active && loopArmed;
            if (lp) while (a >= L.e) a -= L.len;
            if (! sourceDone && a >= N) sourceDone = true;
            spawnCountdown -= 1.0;
            if (! sourceDone && spawnCountdown <= 0.0)
            {
                LoopInfo Lc = L; Lc.active = lp;
                spawnGrain (d, p, r, k, Lc);
            }

            int living = 0;
            for (auto& gr : grains)
            {
                if (! gr.active) continue;
                ++living;
                const double sp = gr.c1 + ((double) gr.age + 0.5 - 0.5 * gr.len) * gr.g;
                const float w = hannAt (((float) gr.age + 0.5f) / (float) gr.len) * gr.gain;
                auto tap = [&] (double q, float& tl, float& tr)
                {
                    tl = readSample (c0, N, q); tr = readSample (c1, N, q);
                    if (gr.w2 > 0.f)
                    {
                        tl = tl * (1.f - gr.w2) + readSample (c0, N, q - gr.shift) * gr.w2;
                        tr = tr * (1.f - gr.w2) + readSample (c1, N, q - gr.shift) * gr.w2;
                    }
                };
                float sl, sR;
                tap (sp, sl, sR);
                if (gr.wA > 0.f || gr.wB > 0.f)
                {
                    const float wC = 1.f - gr.wA - gr.wB;
                    sl *= wC; sR *= wC;
                    float tl, tr;
                    if (gr.wA > 0.f) { tap (sp + gr.offA, tl, tr); sl += tl * gr.wA; sR += tr * gr.wA; }
                    if (gr.wB > 0.f) { tap (sp + gr.offB, tl, tr); sl += tl * gr.wB; sR += tr * gr.wB; }
                }
                l += sl * w; rr += sR * w;
                if (++gr.age >= gr.len) gr.active = false;
            }
            if (sourceDone && living == 0) { kill(); break; }
            a += p.speedHold ? k : r * k;
        }

        const float amp = env * gainSm;
        if (outR != nullptr) { outL[i] += l * amp; outR[i] += rr * amp; }
        else outL[i] += 0.5f * (l + rr) * amp;
    }
}
}
