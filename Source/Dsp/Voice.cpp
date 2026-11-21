#include "Voice.h"
#include "Interp.h"

namespace oto
{
namespace
{
// The LPC residual engine is used for large pitch-up shifts, where plain PSOLA breaks down. For small shifts and for
// pitch-down the (window-corrected) PSOLA path is at least as good, and for very high voices (above ~300 Hz) the LPC
// envelope cannot follow the narrow resonances, so that path is used there as well.
// The choice is made once per note, so vibrato / glide / bend can never switch engines in the middle of a note.
constexpr float kLpMinSemis = 6.0f;
constexpr double kLpMaxF0Period = 0.0033;     // seconds (= 303 Hz)
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
    engInited = false; engDone = false;
    {
        const float semis0 = (float) (note_ - p.rootKey) + p.coarseSemis;
        lpChosen = d.hasLpc() && semis0 >= kLpMinSemis
                && (double) d.source->map.refPeriod >= kLpMaxF0Period * d.source->sampleRate;
    }
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
    // output hop (host samples); the lower limit allows output pitches up to ~10 kHz (it used to stop at ~1.8 kHz)
    const double Tout = std::clamp (Tbase / (k * rEff), 4.0, 16000.0);
    // Formant Hold: the grain always spans ~2 source periods. Widening it to the output hop (pitch down) would let the
    // window contain several source periods, which rebuilds the ORIGINAL pitch instead of lowering it.
    // (limited to 8 hops so that very high pitches never need more grains than there are slots)
    const double half = p.formantHold ? std::min (Tin / g, 8.0 * Tout) : std::max (Tout, Tin / g);
    const int len = std::max (16, (int) std::ceil (2.0 * half));

    Grain* slot = nullptr;
    for (auto& gr : grains) if (! gr.active) { slot = &gr; break; }
    if (slot != nullptr)
    {
        slot->active = true; slot->c1 = c1; slot->g = g; slot->len = len;
        slot->age = firstGrain ? len / 2 : 0;       // first grain starts at its peak (no fade-in of the attack)
        // spawnCountdown is <= 0 here: the grain should have started that many (sub-)samples ago. Keeping the fraction
        // places every grain with sub-sample accuracy instead of on the integer sample grid (pitch jitter at high pitch).
        slot->frac = firstGrain ? 0.0 : std::clamp (-spawnCountdown, 0.0, 1.0);
        slot->gain = (float) std::min (1.0, Tout / half);
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

    const bool useLp = lpChosen && p.formantHold && d.hasLpc();
    const bool useGrains = p.formantHold || p.speedHold || p.flatten;
    usingGrains = useGrains;
    EngineParams ep;
    ep.speedHold = p.speedHold; ep.flatten = p.flatten; ep.denoise = p.denoise; ep.loop = L;
    if (useLp && ! engInited)
    {
        eng.start (usingGrainsPrev ? a : pos);
        engInited = true; engDone = false; engFrac = 0.0; engTail = 0;
        for (int q = 0; q < 4; ++q) ringL[q] = ringR[q] = 0.f;
    }
    else if (! useLp && engInited)
    {
        a = pos = eng.position();
        engInited = false; firstGrain = true; spawnCountdown = 0.0; sourceDone = false;
        for (auto& g : grains) g.active = false;
    }
    usingGrainsPrev = useGrains;
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

        if (useLp)
        {
            // ---------- formant-preserving engine (LPC residual PSOLA) ----------
            engFrac += k;
            while (engFrac >= 1.0)
            {
                engFrac -= 1.0;
                float nl = 0.f, nr = 0.f;
                if (! engDone && ! eng.step (d, ep, r, nl, nr)) engDone = true;
                ringL[0] = ringL[1]; ringL[1] = ringL[2]; ringL[2] = ringL[3]; ringL[3] = nl;
                ringR[0] = ringR[1]; ringR[1] = ringR[2]; ringR[2] = ringR[3]; ringR[3] = nr;
            }
            const float ff = (float) engFrac;
            l  = hermite (ringL[0], ringL[1], ringL[2], ringL[3], ff);
            rr = hermite (ringR[0], ringR[1], ringR[2], ringR[3], ff);
            if (engDone && ++engTail > 8) { kill(); break; }
        }
        else if (! useGrains)
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
                const double ag = (double) gr.age + gr.frac;
                const double sp = gr.c1 + (ag + 0.5 - 0.5 * gr.len) * gr.g;
                const float w = hannAt ((float) ((ag + 0.5) / (double) gr.len)) * gr.gain;
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
