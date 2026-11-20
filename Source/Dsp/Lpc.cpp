#include "Lpc.h"
#include "Biquad.h"
#include "Interp.h"

namespace oto
{
namespace
{
int orderFor (double sr) { return std::clamp (2 * (int) std::lround (sr / 2800.0), 12, kMaxLpcOrder); }

// DC / rumble removal + pre-emphasis (analysis and residual use exactly the same pre-processing)
void preprocess (const std::vector<float>& x, double sr, std::vector<float>& s)
{
    Biquad hp; hp.set (1, sr, 35.0, 0.70710678);
    s.resize (x.size());
    double prev = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double h = hp.process ((double) x[i]);
        s[i] = (float) (h - kPreEmph * prev);
        prev = h;
    }
}

bool levinson (const double* r, int P, float* kOut) noexcept
{
    double a[kMaxLpcOrder + 2] = {}, t[kMaxLpcOrder + 2] = {};
    double err = r[0];
    if (! (err > 0.0)) return false;
    a[0] = 1.0;
    int i = 1;
    for (; i <= P; ++i)
    {
        double acc = r[i];
        for (int j = 1; j < i; ++j) acc += a[j] * r[i - j];
        double k = -acc / err;
        k = std::clamp (k, -0.999, 0.999);
        for (int j = 1; j < i; ++j) t[j] = a[j] + k * a[i - j];
        for (int j = 1; j < i; ++j) a[j] = t[j];
        a[i] = k;
        kOut[i - 1] = (float) k;
        err *= (1.0 - k * k);
        if (err < 1.0e-30) { ++i; break; }
    }
    for (; i <= P; ++i) kOut[i - 1] = 0.f;
    return true;
}

void residualOf (const std::vector<float>& x, double sr, const LpcTrack& lpc, std::vector<float>& e)
{
    std::vector<float> s; preprocess (x, sr, s);
    const int N = (int) s.size(), P = lpc.order;
    e.assign ((size_t) N, 0.f);
    double bp[kMaxLpcOrder + 1] = {};
    float K[kMaxLpcOrder] = {};
    for (int n = 0; n < N; ++n)
    {
        if ((n & 7) == 0) lpcCoefAt (lpc, (double) n, K);
        double f = (double) s[(size_t) n], bn = f;
        for (int m = 1; m <= P; ++m)
        {
            const double bold = bp[m - 1], k = (double) K[m - 1];
            const double fn = f + k * bold;
            const double bm = bold + k * f;
            bp[m - 1] = bn;
            bn = bm;
            f = fn;
        }
        bp[P] = bn;
        e[(size_t) n] = (float) f;
    }
}

}

void lpcCoefAt (const LpcTrack& lpc, double t, float* out) noexcept
{
    if (! lpc.valid()) { for (int i = 0; i < lpc.order; ++i) out[i] = 0.f; return; }
    double u = t / (double) lpc.hop;
    u = std::clamp (u, 0.0, (double) (lpc.numFrames - 1) - 1.0e-9);
    const int j = (int) u; const float f = (float) (u - j);
    const float* a = lpc.k.data() + (size_t) j * (size_t) lpc.order;
    const float* b = a + lpc.order;
    for (int i = 0; i < lpc.order; ++i) out[i] = a[i] + (b[i] - a[i]) * f;
}

void analyzeLpc (const std::vector<float>& mono, double sr, PitchMap& map, LpcTrack& lpc)
{
    lpc = LpcTrack();
    map.gci.clear(); map.gl.clear(); map.gr.clear();
    const int N = (int) mono.size();
    if (N < 2048 || map.marks.empty()) return;

    const int P = orderFor (sr);
    const int H = std::max (32, (int) std::lround (0.004 * sr));
    const int F = N / H + 2;
    lpc.order = P; lpc.hop = H; lpc.numFrames = F;
    lpc.k.assign ((size_t) F * (size_t) P, 0.f);

    std::vector<float> s; preprocess (mono, sr, s);
    std::vector<double> seg;
    double r[kMaxLpcOrder + 1], lagWin[kMaxLpcOrder + 1];
    for (int i = 0; i <= P; ++i) { const double w = 2.0 * kPi * 40.0 * i / sr; lagWin[i] = std::exp (-0.5 * w * w); }

    const int wMin = (int) (0.012 * sr), wMax = (int) (0.032 * sr);
    for (int f = 0; f < F; ++f)
    {
        const int c = f * H;
        const int mi = map.nearest ((double) c);
        const bool voiced = mi >= 0 && map.voiced[(size_t) mi];
        int W = voiced ? (int) std::lround (3.0 * map.periods[(size_t) mi]) : (int) (0.020 * sr);
        W = std::clamp (W, wMin, wMax) | 1;
        const int a0 = c - W / 2;
        seg.assign ((size_t) W, 0.0);
        for (int i = 0; i < W; ++i)
        {
            const int n = a0 + i;
            if (n < 0 || n >= N) continue;
            seg[(size_t) i] = (double) s[(size_t) n] * (0.5 - 0.5 * std::cos (2.0 * kPi * (i + 0.5) / W));
        }
        for (int l = 0; l <= P; ++l)
        {
            double acc = 0.0;
            for (int i = l; i < W; ++i) acc += seg[(size_t) i] * seg[(size_t) (i - l)];
            r[l] = acc * lagWin[l];
        }
        if (r[0] < 1.0e-9) continue;
        r[0] = r[0] * 1.0001 + 1.0e-12;
        levinson (r, P, lpc.k.data() + (size_t) f * (size_t) P);
    }

    // ---- refine the pulse positions on the residual ----
    std::vector<float> e; residualOf (mono, sr, lpc, e);
    std::vector<float> es ((size_t) N);
    for (int i = 0; i < N; ++i)
    {
        const float a = std::abs (e[(size_t) std::max (0, i - 1)]), b = std::abs (e[(size_t) i]), c = std::abs (e[(size_t) std::min (N - 1, i + 1)]);
        es[(size_t) i] = 0.25f * (a + 2.f * b + c);
    }
    const size_t n = map.marks.size();
    map.gci.resize (n); map.gl.resize (n); map.gr.resize (n);
    for (size_t i = 0; i < n; ++i) map.gci[i] = (float) map.marks[i];

    // Track the glottal pulse through each voiced run. The offset between the waveform mark and the residual pulse is
    // found once per run (profile summed over several periods) and then followed with a small search window, so the
    // pulse position never flips between two features of the residual.
    size_t s0 = 0;
    while (s0 < n)
    {
        if (! map.voiced[s0]) { ++s0; continue; }
        size_t s1 = s0; while (s1 + 1 < n && map.voiced[s1 + 1]) ++s1;
        const size_t cnt = std::min<size_t> (s1 - s0 + 1, 7);
        const int T0 = (int) map.periods[s0];
        const int half = std::max (2, T0 / 2);
        double bestSum = -1.0; int bestD = 0;
        for (int dlt = -half; dlt < half; ++dlt)
        {
            double sum = 0.0;
            for (size_t q = 0; q < cnt; ++q) { const int pos = map.marks[s0 + q] + dlt; if (pos >= 0 && pos < N) sum += es[(size_t) pos]; }
            if (sum > bestSum) { bestSum = sum; bestD = dlt; }
        }
        double dSmooth = (double) bestD;
        for (size_t k = s0; k <= s1; ++k)
        {
            const int m = map.marks[k]; const float T = map.periods[k];
            const int rad = std::max (2, (int) (0.12f * T));
            const int c0 = m + (int) std::lround (dSmooth);
            const int lo = std::max (0, c0 - rad), hi = std::min (N - 1, c0 + rad);
            int best = std::clamp (c0, 0, N - 1); float bv = -1.f;
            for (int q = lo; q <= hi; ++q) if (es[(size_t) q] > bv) { bv = es[(size_t) q]; best = q; }
            const int wl = std::max (0, m - (int) T), wh = std::min (N - 1, m + (int) T);
            double mean = 0.0; for (int q = wl; q <= wh; ++q) mean += es[(size_t) q];
            mean /= (double) (wh - wl + 1);
            if (bv > 2.0 * mean) { map.gci[k] = (float) best; dSmooth = 0.7 * dSmooth + 0.3 * (double) (best - m); }
            else map.gci[k] = (float) std::clamp (c0, 0, N - 1);
        }
        s0 = s1 + 1;
    }
    for (size_t i = 1; i < n; ++i)    // keep the order, fall back to the original marks where it would break
        if (map.gci[i] <= map.gci[i - 1] + 2.f) { map.gci[i] = (float) map.marks[i]; if (map.gci[i] <= map.gci[i - 1]) map.gci[i] = map.gci[i - 1] + 1.f; }
    for (size_t i = 0; i < n; ++i)
    {
        if (! map.voiced[i]) { map.gl[i] = map.gr[i] = map.defaultPeriod; continue; }
        const float T = map.periods[i];
        float nx = -1.f, pv = -1.f;
        if (i + 1 < n && map.voiced[i + 1]) { const float d = map.gci[i + 1] - map.gci[i]; if (d > 0.6f * T && d < 1.6f * T) nx = d; }
        if (i > 0 && map.voiced[i - 1])     { const float d = map.gci[i] - map.gci[i - 1]; if (d > 0.6f * T && d < 1.6f * T) pv = d; }
        map.gr[i] = nx > 0.f ? nx : (pv > 0.f ? pv : T);
        map.gl[i] = pv > 0.f ? pv : (nx > 0.f ? nx : T);
    }
}

void computeResiduals (SampleData& d)
{
    d.residual.clear();
    if (d.source == nullptr || ! d.source->lpc.valid() || d.channels.empty()) return;
    d.residual.resize (d.channels.size());
    for (size_t c = 0; c < d.channels.size(); ++c)
        residualOf (d.channels[c], d.source->sampleRate, d.source->lpc, d.residual[c]);
}
}
