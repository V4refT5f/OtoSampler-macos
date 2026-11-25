// SPDX-License-Identifier: AGPL-3.0-only
#include "Additive.h"
#include "Fft.h"
#include "Interp.h"
#include <complex>

namespace oto
{
namespace
{
constexpr double kTwoPi = 6.283185307179586;

inline float wrapCycles (float v) noexcept { return v - std::floor (v + 0.5f); }
inline double wrapCycles (double v) noexcept { return v - std::floor (v + 0.5); }

struct SinTab
{
    float t[2050];
    SinTab() { for (int i = 0; i < 2050; ++i) t[i] = (float) std::cos (kTwoPi * (double) i / 2048.0); }   // partials are cosines (the analysis measures cosine phase)
};
const SinTab& sinTab() { static const SinTab s; return s; }
}

// =====================================================================================================================
// synthesis helpers
// =====================================================================================================================
void evalPartials (const AdditiveModel& m, const HarmonicChannel& c, double pos, const EvalCfg& cfg,
                   EvalScratch& sc, PartialSet& out) noexcept
{
    const int S = m.stride;
    const double fp = pos / (double) m.hop;
    int f; float w;
    if (fp <= 0.0) { f = 0; w = 0.f; }
    else if (fp >= (double) (m.numFrames - 1)) { f = m.numFrames - 2; w = 1.f; }
    else { f = (int) fp; w = (float) (fp - (double) f); }

    const float* A0 = &c.amp[(size_t) f * (size_t) S];
    const float* A1 = A0 + S;
    const float* P0 = &c.ps[(size_t) f * (size_t) S];
    const float* P1 = P0 + S;
    const float t0 = c.tau[(size_t) f], t1 = c.tau[(size_t) f + 1];
    constexpr float eps = 1.0e-9f;

    out.n = cfg.n;
    if (! cfg.formantHold)
    {
        const int n = std::min (cfg.n, S);
        for (int j = 0; j < n; ++j)
        {
            const float a0 = A0[j], a1 = A1[j];
            if (a0 <= eps && a1 <= eps) { out.amp[j] = 0.f; out.ph[j] = 0.f; continue; }
            const float h = (float) (j + 1);
            const float p0 = wrapCycles (P0[j] - h * t0), p1 = wrapCycles (P1[j] - h * t1);
            out.amp[j] = a0 + (a1 - a0) * w;
            if (a0 <= eps) out.ph[j] = p1;
            else if (a1 <= eps) out.ph[j] = p0;
            else out.ph[j] = p0 + w * wrapCycles (p1 - p0);
        }
        for (int j = n; j < cfg.n; ++j) { out.amp[j] = 0.f; out.ph[j] = 0.f; }
        return;
    }

    // ---- formant hold: envelope (amplitude and phase shape over frequency) of the interpolated frame ----
    const float tau = t0 + w * wrapCycles (t1 - t0);
    for (int j = 0; j < S; ++j)
    {
        const float a0 = A0[j], a1 = A1[j];
        sc.af[j] = a0 + (a1 - a0) * w;
        if (a0 <= eps) sc.psi[j] = P1[j];
        else if (a1 <= eps) sc.psi[j] = P0[j];
        else sc.psi[j] = P0[j] + w * wrapCycles (P1[j] - P0[j]);
    }
    for (int h = 1; h <= cfg.n; ++h)
    {
        const float q = (float) h * cfg.rho;        // index of the same frequency in the source
        float a, ps;
        if (q > (float) S) { out.amp[h - 1] = 0.f; out.ph[h - 1] = 0.f; continue; }
        if (q <= 1.f) { a = sc.af[0]; ps = sc.psi[0]; }
        else
        {
            const int j = (int) q;                   // 1-based index of the lower neighbour
            const float fr = q - (float) j;
            const int i0 = j - 1, i1 = std::min (j, S - 1);
            a  = sc.af[i0]  + (sc.af[i1]  - sc.af[i0])  * fr;
            ps = sc.psi[i0] + (sc.psi[i1] - sc.psi[i0]) * fr;
        }
        out.amp[h - 1] = a * cfg.ampScale;
        out.ph[h - 1] = wrapCycles (ps - (float) h * tau);
    }
}

void blendSets (PartialSet& a, const PartialSet& b, float w2, double dFrac) noexcept
{
    constexpr float eps = 1.0e-9f;
    const float w1 = 1.f - w2;
    for (int j = 0; j < a.n; ++j)
    {
        const float a1 = a.amp[j], a2 = b.amp[j];
        const float p1 = a.ph[j];
        const float p2 = (float) wrapCycles ((double) b.ph[j] - (double) (j + 1) * dFrac);
        const float d = wrapCycles (p2 - p1);
        a.amp[j] = a1 * w1 + a2 * w2;
        if (a1 <= eps) a.ph[j] = p1 + d;
        else if (a2 > eps) a.ph[j] = p1 + w2 * d;
    }
}

void averageSets (PartialSet& out, const PartialSet& c, const PartialSet& a, double dA, float wA,
                  const PartialSet& b, double dB, float wB) noexcept
{
    const float wC = 1.f - wA - wB;
    out.n = c.n;
    for (int j = 0; j < c.n; ++j)
    {
        const double h = (double) (j + 1);
        const double pc = (double) c.ph[j];
        const double pa = wrapCycles ((double) a.ph[j] - h * dA);
        const double pb = wrapCycles ((double) b.ph[j] - h * dB);
        const double re = wC * c.amp[j] * std::cos (kTwoPi * pc) + wA * a.amp[j] * std::cos (kTwoPi * pa) + wB * b.amp[j] * std::cos (kTwoPi * pb);
        const double im = wC * c.amp[j] * std::sin (kTwoPi * pc) + wA * a.amp[j] * std::sin (kTwoPi * pa) + wB * b.amp[j] * std::sin (kTwoPi * pb);
        out.amp[j] = (float) std::sqrt (re * re + im * im);
        out.ph[j] = out.amp[j] > 1.0e-9f ? (float) (std::atan2 (im, re) / kTwoPi) : c.ph[j];
    }
}

void bankAdd (const float* a0, const float* a1, const float* p0, const float* inc, int count, int len, float* out, float cull) noexcept
{
    const float* tab = sinTab().t;
    const float invLen = 1.f / (float) std::max (1, len);
    for (int j = 0; j < count; ++j)
    {
        float amp = a0[j];
        const float end = a1[j];
        if (amp < cull && end < cull) continue;
        const float dA = (end - amp) * invLen;
        const float ph = p0[j], ic = inc[j];
        for (int i = 0; i < len; ++i)
        {
            float x = ph + ic * (float) i;
            const int xi = (int) x;
            x -= (float) xi;
            if (x < 0.f) x += 1.f;
            const float t = x * 2048.f;
            const int k = (int) t;
            const float fr = t - (float) k;
            out[i] += amp * (tab[k] + (tab[k + 1] - tab[k]) * fr);
            amp += dA;
        }
    }
}

// =====================================================================================================================
// analysis
// =====================================================================================================================
namespace
{
// Re-synthesises one channel of the model at its original pitch (used to build the residual)
void renderOriginal (const AdditiveModel& M, int ch, int N, std::vector<float>& out)
{
    out.assign ((size_t) N, 0.f);
    const auto& hc = M.data->ch[(size_t) ch];
    const int S = M.stride;
    auto s0 = std::make_unique<PartialSet>(), s1 = std::make_unique<PartialSet>();
    auto sc = std::make_unique<EvalScratch>();
    float pst[kMaxOutPartials], pinc[kMaxOutPartials], tmp[kChunk];

    for (int pos = 0; pos < N; pos += kChunk)
    {
        const int len = std::min (kChunk, N - pos);
        const double f0m = std::max ((double) M.f0At ((double) pos), (double) M.f0At ((double) (pos + len)));
        EvalCfg cfg;
        cfg.n = std::max (0, std::min (S, (int) std::floor (0.49 / std::max (1.0e-5, f0m))));
        evalPartials (M, hc, (double) pos, cfg, *sc, *s0);
        evalPartials (M, hc, (double) (pos + len), cfg, *sc, *s1);

        const double th0 = M.thetaAt ((double) pos), dth = M.thetaAt ((double) (pos + len)) - th0;
        const double th0f = th0 - std::floor (th0);
        for (int j = 0; j < cfg.n; ++j)
        {
            const double h = (double) (j + 1);
            double p = h * th0f + (double) s0->ph[j];
            p -= std::floor (p);
            pst[j] = (float) p;
            pinc[j] = (float) ((h * dth + (double) wrapCycles (s1->ph[j] - s0->ph[j])) / (double) len);
        }
        for (int i = 0; i < len; ++i) tmp[i] = 0.f;
        bankAdd (s0->amp, s1->amp, pst, pinc, cfg.n, len, tmp, 1.0e-7f);
        for (int i = 0; i < len; ++i) out[(size_t) (pos + i)] = tmp[i];
    }
}

void integrateTheta (AdditiveModel& M)
{
    M.theta[0] = 0.0;
    for (int f = 0; f + 1 < M.numFrames; ++f)
        M.theta[(size_t) f + 1] = M.theta[(size_t) f] + 0.5 * (double) M.hop * ((double) M.f0[(size_t) f] + (double) M.f0[(size_t) f + 1]);
}

// unvoiced frames take the fundamental of the nearest voiced frame (keeps theta smooth; their partials are silent)
void fillUnvoiced (AdditiveModel& M, float fallback)
{
    const int F = M.numFrames;
    float last = 0.f;
    for (int f = 0; f < F; ++f)
    {
        if (M.voiced[(size_t) f]) last = M.f0[(size_t) f];
        else if (last > 0.f) M.f0[(size_t) f] = last;
        else M.f0[(size_t) f] = 0.f;
    }
    float next = 0.f;
    for (int f = F - 1; f >= 0; --f)
    {
        if (M.voiced[(size_t) f]) next = M.f0[(size_t) f];
        else if (M.f0[(size_t) f] <= 0.f) M.f0[(size_t) f] = next > 0.f ? next : fallback;
    }
}

void buildTimeline (AdditiveModel& M, const PitchMap& map, int N, double sr, int minHop = 0)
{
    const int hopBase = std::max (32, (int) std::lround (sr * 0.0015));
    M.hop = std::max ({ hopBase, (N + 15999) / 16000, minHop });
    M.numFrames = N / M.hop + 2;
    const int F = M.numFrames;
    M.f0.assign ((size_t) F, 0.f);
    M.theta.assign ((size_t) F, 0.0);
    M.voiced.assign ((size_t) F, 0);

    const int nm = (int) map.marks.size();
    std::vector<float> sp ((size_t) nm, 0.f), med ((size_t) nm, 0.f);
    for (int i = 0; i < nm; ++i)
    {
        if (! map.voiced[(size_t) i]) continue;
        float v[5]; int n = 0;
        for (int k = -2; k <= 2; ++k)
        {
            const int j = i + k;
            if (j >= 0 && j < nm && map.voiced[(size_t) j]) v[n++] = map.periods[(size_t) j];
        }
        std::sort (v, v + n);
        med[(size_t) i] = v[n / 2];
    }
    for (int i = 0; i < nm; ++i)
    {
        if (! map.voiced[(size_t) i]) continue;
        float sum = 0.f; int cnt = 0;
        for (int k = -1; k <= 1; ++k)
        {
            const int j = i + k;
            if (j >= 0 && j < nm && map.voiced[(size_t) j]) { sum += med[(size_t) j]; ++cnt; }
        }
        sp[(size_t) i] = sum / (float) std::max (1, cnt);
    }

    for (int f = 0; f < F; ++f)
    {
        const int tf = f * M.hop;
        const int j = (int) (std::lower_bound (map.marks.begin(), map.marks.end(), tf) - map.marks.begin());
        const int i1 = j, i0 = j - 1;
        const bool v0 = i0 >= 0 && map.voiced[(size_t) i0], v1 = i1 < nm && map.voiced[(size_t) i1];
        float T = 0.f;
        if (v0 && v1)
        {
            const double m0 = map.marks[(size_t) i0], m1 = map.marks[(size_t) i1];
            const double u = m1 > m0 ? ((double) tf - m0) / (m1 - m0) : 0.0;
            T = sp[(size_t) i0] + (sp[(size_t) i1] - sp[(size_t) i0]) * (float) u;
        }
        else if (v0 && (double) tf - map.marks[(size_t) i0] < 0.6 * sp[(size_t) i0]) T = sp[(size_t) i0];
        else if (v1 && (double) map.marks[(size_t) i1] - tf < 0.6 * sp[(size_t) i1]) T = sp[(size_t) i1];
        if (T >= 16.f && T <= 8192.f) { M.voiced[(size_t) f] = 1; M.f0[(size_t) f] = 1.f / T; }
    }
    fillUnvoiced (M, 1.f / map.defaultPeriod);
    M.refF0 = 1.f / std::max (16.f, map.refPeriod);
    integrateTheta (M);
}

// Harmonic analysis of every voiced frame. The signal around the frame centre is re-sampled in the phase domain of
// the fundamental (a fixed number of samples per cycle), so that a window of exactly 4 cycles holds every harmonic h
// at FFT bin 4h, whatever the pitch does inside the window.
// Two real signals share one complex FFT (the two channels of a frame, or two frames of a mono sample).
void analysisPass (const AdditiveModel& M, const std::vector<const float*>& x, int N, int Mc,
                   std::vector<std::vector<float>>& amp, std::vector<std::vector<float>>& ph)
{
    const int nch = (int) x.size(), S = M.stride, NF = 4 * Mc, F = M.numFrames;
    amp.assign ((size_t) nch, std::vector<float> ((size_t) F * (size_t) S, 0.f));
    ph.assign ((size_t) nch, std::vector<float> ((size_t) F * (size_t) S, 0.f));

    // phase of the fundamental at every source sample (with margins), for the inverse mapping phase -> time
    const int pad = 16400;
    const size_t TN = (size_t) N + 2 * (size_t) pad;
    std::vector<double> th (TN);
    for (size_t i = 0; i < TN; ++i) th[i] = M.thetaAt ((double) ((long long) i - pad));

    struct Job { int f, c, Hf; };
    std::vector<Job> jobs;
    for (int f = 0; f < F; ++f)
    {
        if (! M.voiced[(size_t) f]) continue;
        const int Hf = std::min (S, (int) std::floor (0.49 / (double) M.f0[(size_t) f]));
        if (Hf < 1) continue;
        for (int c = 0; c < nch; ++c) jobs.push_back ({ f, c, Hf });
    }

    const Fft fft (NF);
    std::vector<double> win ((size_t) NF);
    for (int i = 0; i < NF; ++i) win[(size_t) i] = 0.5 - 0.5 * std::cos (kTwoPi * (double) i / (double) NF);
    std::vector<std::complex<double>> buf ((size_t) NF);
    std::vector<double> tpos[2] = { std::vector<double> ((size_t) NF), std::vector<double> ((size_t) NF) };

    auto mapFrame = [&] (int f, std::vector<double>& out)
    {
        const double thc = M.theta[(size_t) f];
        size_t n = (size_t) (std::lower_bound (th.begin(), th.end(), thc - (double) (NF / 2) / (double) Mc) - th.begin());
        n = std::clamp<size_t> (n, 1, TN - 1);
        for (int i = 0; i < NF; ++i)
        {
            const double target = thc + (double) (i - NF / 2) / (double) Mc;
            while (n + 1 < TN && th[n] < target) ++n;
            const double d = th[n] - th[n - 1];
            const double u = d > 1.0e-12 ? (target - th[n - 1]) / d : 0.0;
            out[(size_t) i] = (double) (n - 1) + u - (double) pad;
        }
    };

    for (size_t j = 0; j < jobs.size(); j += 2)
    {
        const Job j1 = jobs[j];
        const bool two = j + 1 < jobs.size();
        const Job j2 = two ? jobs[j + 1] : j1;

        mapFrame (j1.f, tpos[0]);
        if (two && j2.f != j1.f) mapFrame (j2.f, tpos[1]);
        const std::vector<double>& t2 = (two && j2.f != j1.f) ? tpos[1] : tpos[0];
        for (int i = 0; i < NF; ++i)
        {
            const double l = readSample (x[(size_t) j1.c], N, tpos[0][(size_t) i]);
            const double r = two ? readSample (x[(size_t) j2.c], N, t2[(size_t) i]) : 0.0;
            buf[(size_t) i] = std::complex<double> (l * win[(size_t) i], r * win[(size_t) i]);
        }
        fft.forward (buf.data());

        const Job jb[2] = { j1, j2 };
        for (int q = 0; q < (two ? 2 : 1); ++q)
        {
            const double thc = M.theta[(size_t) jb[q].f];
            for (int h = 1; h <= jb[q].Hf; ++h)
            {
                const int k = 4 * h;
                const std::complex<double> X = buf[(size_t) k], Y = std::conj (buf[(size_t) ((NF - k) % NF)]);
                const std::complex<double> Z = q == 0 ? 0.5 * (X + Y) : (X - Y) * std::complex<double> (0.0, -0.5);
                const size_t idx = (size_t) jb[q].f * (size_t) S + (size_t) (h - 1);
                amp[(size_t) jb[q].c][idx] = (float) (4.0 * std::abs (Z) / (double) NF);
                ph[(size_t) jb[q].c][idx] = (float) wrapCycles (std::arg (Z) / kTwoPi - (double) h * thc);
            }
        }
    }
}

// The phase of the partials is measured against theta. If the pitch track is a little off, the phase of partial h
// drifts at h times the frequency error. That drift is used to correct the pitch track.
void refinePitch (AdditiveModel& M, const std::vector<std::vector<float>>& amp, const std::vector<std::vector<float>>& ph, float fallback)
{
    const int F = M.numFrames, S = M.stride, nch = (int) amp.size();
    std::vector<double> dp ((size_t) std::max (0, F - 1), 0.0);
    std::vector<uint8_t> have ((size_t) std::max (0, F - 1), 0);
    for (int f = 0; f + 1 < F; ++f)
    {
        if (! M.voiced[(size_t) f] || ! M.voiced[(size_t) f + 1]) continue;
        const int Hm = std::min (24, S);
        double num = 0.0, den = 0.0;
        for (int c = 0; c < nch; ++c)
            for (int h = 1; h <= Hm; ++h)
            {
                const size_t i0 = (size_t) f * (size_t) S + (size_t) (h - 1), i1 = i0 + (size_t) S;
                const float a0 = amp[(size_t) c][i0], a1 = amp[(size_t) c][i1];
                if (a0 < 1.0e-4f || a1 < 1.0e-4f) continue;
                const double w = (double) a0 * a1, xh = (double) h * M.hop;
                const double d = wrapCycles ((double) ph[(size_t) c][i1] - (double) ph[(size_t) c][i0]);
                num += w * xh * d;
                den += w * xh * xh;
            }
        if (den > 0.0) { dp[(size_t) f] = num / den; have[(size_t) f] = 1; }
    }

    std::vector<double> delta ((size_t) F, 0.0);
    std::vector<uint8_t> ok ((size_t) F, 0);
    for (int f = 0; f < F; ++f)
    {
        if (! M.voiced[(size_t) f]) continue;
        double sum = 0.0; int cnt = 0;
        if (f > 0 && have[(size_t) f - 1]) { sum += dp[(size_t) f - 1]; ++cnt; }
        if (f + 1 < F && have[(size_t) f]) { sum += dp[(size_t) f]; ++cnt; }
        if (cnt > 0) { delta[(size_t) f] = sum / cnt; ok[(size_t) f] = 1; }
    }
    // smooth over 5 frames
    std::vector<double> sm = delta;
    for (int f = 0; f < F; ++f)
    {
        if (! ok[(size_t) f]) continue;
        double sum = 0.0; int cnt = 0;
        for (int k = -2; k <= 2; ++k)
        {
            const int g = f + k;
            if (g >= 0 && g < F && ok[(size_t) g]) { sum += delta[(size_t) g]; ++cnt; }
        }
        sm[(size_t) f] = sum / cnt;
    }
    for (int f = 0; f < F; ++f)
    {
        if (! ok[(size_t) f]) continue;
        const double lim = 0.02 * (double) M.f0[(size_t) f];
        M.f0[(size_t) f] += (float) std::clamp (sm[(size_t) f], -lim, lim);
    }
    fillUnvoiced (M, fallback);
    integrateTheta (M);
}

// raw partial phases -> (pulse offset tau, phase shape ps)
void convertPhases (const AdditiveModel& M, const std::vector<float>& ampRaw, const std::vector<float>& phRaw, HarmonicChannel& out)
{
    const int F = M.numFrames, S = M.stride;
    for (int f = 0; f < F; ++f)
    {
        if (! M.voiced[(size_t) f]) continue;
        const size_t base = (size_t) f * (size_t) S;
        const int Hf = std::min (S, (int) std::floor (0.49 / (double) M.f0[(size_t) f]));

        std::complex<double> D (0.0, 0.0);
        for (int h = 1; h < Hf; ++h)
        {
            const float a0 = ampRaw[base + (size_t) h - 1], a1 = ampRaw[base + (size_t) h];
            const double d = (double) phRaw[base + (size_t) h] - (double) phRaw[base + (size_t) h - 1];
            D += (double) a0 * a1 * std::polar (1.0, kTwoPi * d);
        }
        const double tau = std::abs (D) > 1.0e-20 ? -std::arg (D) / kTwoPi : 0.0;
        out.tau[(size_t) f] = (float) tau;

        double psPrev = 0.0;
        bool first = true;
        for (int h = 1; h <= S; ++h)
        {
            const size_t idx = base + (size_t) h - 1;
            const float a = h <= Hf ? ampRaw[idx] : 0.f;
            out.amp[idx] = a;
            if (a < 1.0e-6f) { out.ps[idx] = (float) psPrev; continue; }
            const double target = (double) phRaw[idx] + (double) h * tau;
            double ps;
            if (first) { ps = target - std::floor (target + 0.5); first = false; }
            else { double d = target - psPrev; d -= std::floor (d + 0.5); ps = psPrev + d; }
            psPrev = ps;
            out.ps[idx] = (float) ps;
        }
    }
}
}

std::shared_ptr<AdditiveModel> analyzeAdditive (const std::vector<std::vector<float>>& chans, double sr, const PitchMap& map)
{
    auto M = std::make_shared<AdditiveModel>();
    int nch = (int) std::min<size_t> (2, chans.size());
    const int N = chans.empty() ? 0 : (int) chans[0].size();
    if (nch == 0 || N < 64) return M;
    if (nch == 2)   // two identical channels are analysed (and played) only once
    {
        bool same = chans[1].size() == chans[0].size();
        for (int i = 0; same && i < N; ++i) same = std::abs (chans[0][(size_t) i] - chans[1][(size_t) i]) < 1.0e-6f;
        if (same) nch = 1;
    }

    buildTimeline (*M, map, N, sr);
    const float fallback = 1.f / map.defaultPeriod;

    bool anyVoiced = false;
    auto chooseStride = [&]
    {
        int hMax = 1;
        anyVoiced = false;
        for (int f = 0; f < M->numFrames; ++f)
            if (M->voiced[(size_t) f]) { anyVoiced = true; hMax = std::max (hMax, (int) std::floor (0.49 / (double) M->f0[(size_t) f])); }
        M->stride = anyVoiced ? std::clamp (hMax, 1, kMaxPartials) : 1;
    };
    chooseStride();
    // memory limit: frames * partials per channel. Long samples with a low pitch get a larger frame spacing.
    constexpr double kMaxCells = 6.0e6;
    if ((double) M->numFrames * (double) M->stride > kMaxCells)
    {
        buildTimeline (*M, map, N, sr, (int) std::ceil ((double) N * (double) M->stride / kMaxCells));
        chooseStride();
    }

    auto data = std::make_shared<AdditiveData>();
    data->ch.resize ((size_t) nch);
    data->residual.resize ((size_t) nch);
    const size_t FS = (size_t) M->numFrames * (size_t) M->stride;
    for (auto& c : data->ch)
    {
        c.amp.assign (FS, 0.f);
        c.ps.assign (FS, 0.f);
        c.tau.assign ((size_t) M->numFrames, 0.f);
    }

    if (anyVoiced && N >= 1024)
    {
        std::vector<const float*> x;
        for (int c = 0; c < nch; ++c) x.push_back (chans[(size_t) c].data());
        int Mc = 16;
        while (Mc < 2 * M->stride) Mc <<= 1;

        std::vector<std::vector<float>> amp, ph;
        analysisPass (*M, x, N, Mc, amp, ph);        // 1st pass: measure how far the pitch track is off
        refinePitch (*M, amp, ph, fallback);
        analysisPass (*M, x, N, Mc, amp, ph);        // 2nd pass with the corrected pitch track
        for (int c = 0; c < nch; ++c) convertPhases (*M, amp[(size_t) c], ph[(size_t) c], data->ch[(size_t) c]);
    }
    M->data = data;

    // residual = original - partials (at the original pitch)
    std::vector<float> synth;
    for (int c = 0; c < nch; ++c)
    {
        renderOriginal (*M, c, N, synth);
        auto& r = data->residual[(size_t) c];
        r.resize ((size_t) N);
        for (int i = 0; i < N; ++i) r[(size_t) i] = chans[(size_t) c][(size_t) i] - synth[(size_t) i];
    }
    return M;
}

std::shared_ptr<const AdditiveData> filterAdditive (const AdditiveModel& M, const FilterSettings& fs, double sr)
{
    if (! M.valid()) return nullptr;
    const bool useHp = fs.on && fs.hp > 20.5f;
    const bool useLp = fs.on && fs.lp < 19990.f && fs.lp < (float) (sr * 0.45);
    if (! useHp && ! useLp) return M.data;

    auto out = std::make_shared<AdditiveData> (*M.data);

    // magnitude / unwrapped phase of the filter on a fine grid, read back by interpolation
    constexpr int G = 4096;
    std::vector<float> gmag ((size_t) G + 2), gph ((size_t) G + 2);
    ButterworthCascade hp, lp;
    hp.set (1, sr, fs.hp);
    lp.set (0, sr, fs.lp);
    double prev = 0.0;
    for (int g = 0; g <= G; ++g)
    {
        const double w = 3.14159265358979323846 * (double) g / (double) G;
        std::complex<double> H (1.0, 0.0);
        if (useHp) H *= hp.response (w);
        if (useLp) H *= lp.response (w);
        double p = std::arg (H);
        p += kTwoPi * std::round ((prev - p) / kTwoPi);
        prev = p;
        gmag[(size_t) g] = (float) std::abs (H);
        gph[(size_t) g] = (float) (p / kTwoPi);
    }
    gmag[(size_t) G + 1] = gmag[(size_t) G];
    gph[(size_t) G + 1] = gph[(size_t) G];

    const int F = M.numFrames, S = M.stride;
    for (auto& c : out->ch)
        for (int f = 0; f < F; ++f)
        {
            const double f0c = (double) M.f0[(size_t) f];
            const size_t base = (size_t) f * (size_t) S;
            for (int j = 0; j < S; ++j)
            {
                if (c.amp[base + (size_t) j] <= 0.f) continue;
                const double gpos = (double) (j + 1) * f0c * 2.0 * (double) G;   // omega / pi * G  (f0c is in cycles per sample)
                if (gpos >= (double) G) { c.amp[base + (size_t) j] = 0.f; continue; }
                const int g0 = (int) gpos;
                const float fr = (float) (gpos - (double) g0);
                c.amp[base + (size_t) j] *= gmag[(size_t) g0] + (gmag[(size_t) g0 + 1] - gmag[(size_t) g0]) * fr;
                c.ps[base + (size_t) j] += gph[(size_t) g0] + (gph[(size_t) g0 + 1] - gph[(size_t) g0]) * fr;
            }
        }
    for (auto& r : out->residual) applyPreFilter (r, sr, fs);
    return out;
}
}
