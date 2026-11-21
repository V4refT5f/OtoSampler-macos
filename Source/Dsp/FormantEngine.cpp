#include "FormantEngine.h"

namespace oto
{
namespace
{
// Output level of a grain relative to the source for a pitch-up factor rho = Tin / Tout.
// Up: between equal power (rho^-0.5) and equal harmonic level (rho^-1). Down: unchanged pulse amplitude
// (the peak level of the source is kept).
inline float levelFor (double rho) noexcept
{
    return rho > 1.0 ? (float) std::pow (rho, -0.75) : 1.f;
}
}

void FormantEngine::start (double startPos) noexcept
{
    a = startPos; nextCenter = 0.0; lastHl = 0.0; filtOff = filtOffTarget = 0.0;
    sourceDone = false; loopArmed = false; coefCountdown = 0; quiet = 0;
    for (auto& g : grains) g.active = false;
    synL.reset(); synR.reset();
}

void FormantEngine::spawn (const SampleData& d, const EngineParams& p, double r, bool lp) noexcept
{
    const PitchMap& m = d.source->map;
    const LoopInfo& L = p.loop;
    const int N = d.numSamples();
    const double adv = p.speedHold ? 1.0 : r;

    double ac = a + nextCenter * adv;
    if (lp) while (ac >= L.e) ac -= L.len;
    if (ac >= (double) N) { sourceDone = true; return; }
    if (ac < 0.0) ac = 0.0;

    Grain* slot = nullptr;
    for (auto& g : grains) if (! g.active) { slot = &g; break; }

    // ---- choose the source pulses ----
    const int n = (int) m.gci.size();
    int i0 = (int) (std::upper_bound (m.gci.begin(), m.gci.end(), (float) ac) - m.gci.begin()) - 1;
    int i1 = i0 + 1;
    int inear = i0 < 0 ? 0 : (i1 >= n ? i0 : ((ac - m.gci[(size_t) i0] <= m.gci[(size_t) i1] - ac) ? i0 : i1));
    const bool voiced = n > 0 && m.voiced[(size_t) inear];

    double tc[4] = {}; float tw[4] = {}; int nt = 0;
    double hl = (double) m.defaultPeriod, hr = hl;
    if (voiced)
    {
        int idx[2] = { inear, inear }; float wt[2] = { 1.f, 0.f }; int nb = 1;
        hl = (double) m.gl[(size_t) inear]; hr = (double) m.gr[(size_t) inear];
        if (i0 >= 0 && i1 < n && m.voiced[(size_t) i0] && m.voiced[(size_t) i1])
        {
            const double span = (double) (m.gci[(size_t) i1] - m.gci[(size_t) i0]);
            const double per = 0.5 * ((double) m.gr[(size_t) i0] + (double) m.gl[(size_t) i1]);
            if (span > 1.0 && span < 1.7 * per)
            {
                // when a source period is used several times (pitch up), morph between the neighbouring periods
                // instead of repeating one period (repeats make the noise part tonal)
                const double phi = std::clamp ((ac - (double) m.gci[(size_t) i0]) / span, 0.0, 1.0);
                const double rep = std::clamp (r / adv - 1.0, 0.0, 1.0);
                const double pe = std::floor (phi + 0.5) + (phi - std::floor (phi + 0.5)) * rep;
                idx[0] = i0; idx[1] = i1; wt[0] = (float) (1.0 - pe); wt[1] = (float) pe; nb = 2;
                hl = (1.0 - pe) * (double) m.gl[(size_t) i0] + pe * (double) m.gl[(size_t) i1];
                hr = (1.0 - pe) * (double) m.gr[(size_t) i0] + pe * (double) m.gr[(size_t) i1];
            }
        }
        const float wn = p.denoise > 0.f ? std::clamp (p.denoise, 0.f, 1.f) * (1.f / 3.f) : 0.f;
        for (int b = 0; b < nb; ++b)
        {
            if (wt[b] <= 0.f) continue;
            const int ib = idx[b];
            const float T = 0.5f * (m.gl[(size_t) ib] + m.gr[(size_t) ib]);
            float wc = wt[b];
            if (wn > 0.f)
            {
                for (int s = -1; s <= 1; s += 2)
                {
                    const int j = ib + s;
                    if (j < 0 || j >= n || ! m.voiced[(size_t) j]) continue;
                    const double off = (double) m.gci[(size_t) j] - (double) m.gci[(size_t) ib];
                    if (std::abs (off) < 0.5 * T || std::abs (off) > 1.6 * T) continue;
                    // merge with an existing tap on the same pulse
                    int slotT = -1; for (int q = 0; q < nt; ++q) if (tc[q] == (double) m.gci[(size_t) j]) slotT = q;
                    const float w = wt[b] * wn;
                    if (slotT >= 0) tw[slotT] += w;
                    else if (nt < 4) { tc[nt] = (double) m.gci[(size_t) j]; tw[nt] = w; ++nt; }
                    else continue;
                    wc -= w;
                }
            }
            int slotT = -1; for (int q = 0; q < nt; ++q) if (tc[q] == (double) m.gci[(size_t) ib]) slotT = q;
            if (slotT >= 0) tw[slotT] += wc;
            else if (nt < 4) { tc[nt] = (double) m.gci[(size_t) ib]; tw[nt] = wc; ++nt; }
        }
    }
    else { tc[0] = std::floor (ac + 0.5); tw[0] = 1.f; nt = 1; }

    hl = std::max (hl, 8.0); hr = std::max (hr, 8.0);
    const double Tin = 0.5 * (hl + hr);
    const double rEff = voiced ? r : 1.0;
    const double Tbase = (p.flatten && voiced) ? (double) m.refPeriod : hr;   // distance to the next pulse (exact at ratio 1)
    const double minHop = std::max (4.0, 0.0001 * d.source->sampleRate);    // pitches up to ~10 kHz
    const double Tout = std::clamp (Tbase / rEff, minHop, 0.4 * d.source->sampleRate);

    // at very large pitch-up factors limit the overlap (grain length <= 24 output hops) so the grain slots never run out
    const double hlU = std::min (hl, 24.0 * Tout), hrU = std::min (hr, 24.0 * Tout);

    if (slot != nullptr)
    {
        const double S = nextCenter - hlU;                 // ideal start time of the grain
        const int I = (int) std::lround (S);
        slot->active = true; slot->nt = nt;
        for (int q = 0; q < nt; ++q) { slot->tc[q] = tc[q]; slot->tw[q] = tw[q]; }
        slot->hl = hlU; slot->hr = hrU; slot->cpos = nextCenter - (double) I;
        slot->age = -I;
        slot->len = (int) std::ceil (slot->cpos + hrU) + 2;
        slot->gain = levelFor (Tin / Tout);
        slot->w2 = 0.f; slot->shift = 0.0;
        if (p.loop.active && lp && ac >= L.e - L.xf)
        {
            slot->w2 = crossW2 ((ac - (L.e - L.xf)) / L.xf);
            slot->shift = L.len;
        }
    }
    {
        double sw = 0.0, sc = 0.0;
        for (int q = 0; q < nt; ++q) { sw += (double) tw[q]; sc += (double) tw[q] * tc[q]; }
        filtOffTarget = sw > 0.0 ? std::clamp (sc / sw - ac, -2000.0, 2000.0) : 0.0;
    }
    lastHl = hlU;
    nextCenter += Tout;
}

bool FormantEngine::step (const SampleData& d, const EngineParams& p, double r, float& outL, float& outR) noexcept
{
    outL = outR = 0.f;
    const int N = d.numSamples();
    const LoopInfo& L = p.loop;
    if (N < 4 || ! d.hasLpc()) return false;

    if (L.active && ! loopArmed && a < L.e) loopArmed = true;
    const bool lp = L.active && loopArmed;
    if (lp) while (a >= L.e) a -= L.len;
    if (! sourceDone && a >= (double) N) sourceDone = true;

    int guard = 0;
    while (! sourceDone && nextCenter <= lastHl + 1.0 && guard++ < 8) spawn (d, p, r, lp);

    const float* rc0 = d.residual[0].data();
    const float* rc1 = d.residual.size() > 1 ? d.residual[1].data() : rc0;
    const bool stereo = d.residual.size() > 1;

    float el = 0.f, er = 0.f;
    int living = 0;
    for (auto& g : grains)
    {
        if (! g.active) continue;
        ++living;
        if (g.age < 0) { ++g.age; continue; }
        const double x = (double) g.age - g.cpos;
        float w = 0.f;
        if (x < 0.0) { if (x > -g.hl) w = hannAt ((float) (0.5 * (x + g.hl) / g.hl)); }
        else if (x < g.hr) w = hannAt ((float) (0.5 + 0.5 * x / g.hr));
        if (w > 0.f)
        {
            float sl = 0.f, sr = 0.f;
            for (int q = 0; q < g.nt; ++q)
            {
                const double sp = g.tc[q] + x;
                float tl = readSample (rc0, N, sp), tr = stereo ? readSample (rc1, N, sp) : tl;
                if (g.w2 > 0.f)
                {
                    tl = tl * (1.f - g.w2) + readSample (rc0, N, sp - g.shift) * g.w2;
                    tr = stereo ? tr * (1.f - g.w2) + readSample (rc1, N, sp - g.shift) * g.w2 : tl;
                }
                sl += tl * g.tw[q]; sr += tr * g.tw[q];
            }
            const float wg = w * g.gain;
            el += sl * wg; er += sr * wg;
        }
        if (++g.age >= g.len) g.active = false;
    }

    // ---- vocal tract filter, follows the ORIGINAL time line ----
    if (--coefCountdown <= 0)
    {
        coefCountdown = 8;
        filtOff += (filtOffTarget - filtOff) * 0.25;
        const double ta = a + filtOff;
        lpcCoefAt (d.source->lpc, ta, kA);
        if (lp && ta >= L.e - L.xf)
        {
            lpcCoefAt (d.source->lpc, ta - L.len, kB);
            const float w = crossW2 ((ta - (L.e - L.xf)) / L.xf);
            for (int i = 0; i < d.source->lpc.order; ++i) kA[i] += (kB[i] - kA[i]) * w;
        }
    }
    const int P = d.source->lpc.order;
    outL = (float) synL.process (kA, P, (double) el);
    outR = stereo ? (float) synR.process (kA, P, (double) er) : outL;

    a += p.speedHold ? 1.0 : r;
    nextCenter -= 1.0;

    if (sourceDone && living == 0)
    {
        if (std::abs (outL) + std::abs (outR) < 1.0e-5f) { if (++quiet > 512) return false; }
        else quiet = 0;
    }
    return true;
}
}
