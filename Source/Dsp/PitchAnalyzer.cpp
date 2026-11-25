// SPDX-License-Identifier: AGPL-3.0-only
#include "PitchAnalyzer.h"

namespace oto
{
static void fillUnvoiced (PitchMap& map, int N, int defP)
{
    map.marks.clear(); map.voiced.clear(); map.periods.clear();
    for (int p = 0; p < std::max (1, N); p += defP) { map.marks.push_back (p); map.voiced.push_back (0); }
    map.periods.assign (map.marks.size(), (float) defP);
    map.refPeriod = (float) defP;
}

PitchMap analyzePitch (const std::vector<float>& x, double sr)
{
    PitchMap map;
    const int N = (int) x.size();
    map.defaultPeriod = (float) std::max (32.0, 0.005 * sr);
    const int defP = (int) map.defaultPeriod;

    // ---- 1. coarse F0 track on a decimated copy (~11 kHz) ----
    const int D = std::max (1, (int) std::lround (sr / 11025.0));
    const double srD = sr / D;
    const int ND = N / D;
    const int hop = std::max (1, (int) std::lround (0.005 * srD));
    const int lagMin = std::max (3, (int) (srD / 700.0));
    const int lagMax = (int) (srD / 65.0);
    const int win = lagMax * 2;

    if (ND < win + lagMax + 8) { fillUnvoiced (map, N, defP); return map; }

    std::vector<float> d ((size_t) ND);
    for (int i = 0; i < ND; ++i)
    {
        float s = 0.f;
        for (int k = 0; k < D; ++k) s += x[(size_t) (i * D + k)];
        d[(size_t) i] = s / (float) D;
    }

    const int numFrames = ND / hop + 1;
    std::vector<float> fper ((size_t) numFrames, 0.f), fe ((size_t) numFrames, 0.f);
    std::vector<uint8_t> fv ((size_t) numFrames, 0);
    std::vector<double> corr ((size_t) lagMax + 3, 0.0);
    const int sMax = ND - win - lagMax - 3;

    float maxE = 0.f;
    auto frameStart = [&] (int f) { return std::clamp (f * hop - win / 2, 0, sMax); };
    for (int f = 0; f < numFrames; ++f)
    {
        const int s = frameStart (f);
        double e = 0.0;
        for (int i = 0; i < win; ++i) e += (double) d[(size_t) (s + i)] * d[(size_t) (s + i)];
        fe[(size_t) f] = (float) (e / win);
        maxE = std::max (maxE, fe[(size_t) f]);
    }

    for (int f = 0; f < numFrames; ++f)
    {
        if (fe[(size_t) f] < maxE * 1e-3f) continue;
        const int s = frameStart (f);
        const double e0 = (double) fe[(size_t) f] * win;
        for (int lag = lagMin - 1; lag <= lagMax + 1; ++lag)
        {
            double num = 0.0, e1 = 0.0;
            for (int i = 0; i < win; ++i)
            {
                const double a = d[(size_t) (s + i)], b = d[(size_t) (s + i + lag)];
                num += a * b; e1 += b * b;
            }
            corr[(size_t) lag] = num / std::sqrt (e0 * e1 + 1e-12);
        }
        double best = -1.0;
        for (int lag = lagMin; lag <= lagMax; ++lag)
            if (corr[(size_t) lag] >= corr[(size_t) lag - 1] && corr[(size_t) lag] > corr[(size_t) lag + 1])
                best = std::max (best, corr[(size_t) lag]);
        if (best < 0.5) continue;
        int pick = -1;
        for (int lag = lagMin; lag <= lagMax && pick < 0; ++lag)
            if (corr[(size_t) lag] >= corr[(size_t) lag - 1] && corr[(size_t) lag] > corr[(size_t) lag + 1]
                && corr[(size_t) lag] >= 0.88 * best)
                pick = lag;
        if (pick < 0) continue;
        const double y0 = corr[(size_t) pick - 1], y1 = corr[(size_t) pick], y2 = corr[(size_t) pick + 1];
        const double den = y0 - 2.0 * y1 + y2;
        const double off = std::abs (den) > 1e-9 ? 0.5 * (y0 - y2) / den : 0.0;
        fper[(size_t) f] = (float) ((pick + std::clamp (off, -1.0, 1.0)) * D);
        fv[(size_t) f] = 1;
    }

    // median smoothing / outlier rejection
    {
        auto per = fper; auto vo = fv;
        for (int f = 0; f < numFrames; ++f)
        {
            if (! vo[(size_t) f]) continue;
            float v[5]; int n = 0;
            for (int k = -2; k <= 2; ++k)
            {
                const int g = f + k;
                if (g >= 0 && g < numFrames && vo[(size_t) g]) v[n++] = per[(size_t) g];
            }
            std::sort (v, v + n);
            const float med = v[n / 2];
            if (n < 2) { fv[(size_t) f] = 0; continue; }
            if (std::abs (per[(size_t) f] - med) / med > 0.2f) fper[(size_t) f] = med;
        }
    }

    // ---- 2. pitch marks on the full-rate signal ----
    const int hopS = hop * D;
    auto frameAt = [&] (int pos) { return std::min (numFrames - 1, (int) std::lround ((double) pos / hopS)); };

    std::vector<int> marks; std::vector<uint8_t> vflag;
    int next = 0, lastMark = 0, pol = 1; bool lastVoiced = false;
    while (next < N)
    {
        const int f = frameAt (next);
        if (! fv[(size_t) f])
        {
            marks.push_back (next); vflag.push_back (0);
            lastVoiced = false; next += defP; continue;
        }
        const double T = std::clamp ((double) fper[(size_t) f], 16.0, 8000.0);
        int lo, hi; double expected = 0.0;
        if (! lastVoiced)
        {
            lo = next; hi = std::min (N - 1, next + (int) T);
            float mx = -1e30f, mn = 1e30f;
            for (int i = lo; i <= hi; ++i) { mx = std::max (mx, x[(size_t) i]); mn = std::min (mn, x[(size_t) i]); }
            pol = (mx >= -mn) ? 1 : -1;
        }
        else
        {
            lo = lastMark + (int) (0.75 * T); hi = std::min (N - 1, lastMark + (int) (1.25 * T));
            expected = lastMark + T;
        }
        if (lo >= N) break;
        hi = std::max (hi, lo);
        int m = lo; float bestScore = -1e30f;
        for (int i = lo; i <= hi; ++i)
        {
            const float v = (float) pol * x[(size_t) i];
            const float w = lastVoiced ? 1.f - 0.5f * (float) std::abs (i - expected) / (0.25f * (float) T) : 1.f;
            const float score = v > 0.f ? v * w : v;
            if (score > bestScore) { bestScore = score; m = i; }
        }
        marks.push_back (m); vflag.push_back (1);
        lastMark = m; lastVoiced = true;
        next = m + std::max (1, (int) (0.75 * T));
    }
    if (marks.empty()) { fillUnvoiced (map, N, defP); return map; }

    map.marks = std::move (marks);
    map.voiced = std::move (vflag);
    const size_t n = map.marks.size();
    map.periods.assign (n, map.defaultPeriod);
    for (size_t i = 0; i < n; ++i)
    {
        if (! map.voiced[i]) continue;
        float p = map.defaultPeriod;
        if (i + 1 < n && map.voiced[i + 1]) p = (float) (map.marks[i + 1] - map.marks[i]);
        else if (i > 0 && map.voiced[i - 1]) p = (float) (map.marks[i] - map.marks[i - 1]);
        map.periods[i] = std::clamp (p, 16.f, 8192.f);
    }
    {
        std::vector<float> v;
        for (size_t i = 0; i < n; ++i) if (map.voiced[i]) v.push_back (map.periods[i]);
        if (! v.empty()) { std::nth_element (v.begin(), v.begin() + (long) (v.size() / 2), v.end()); map.refPeriod = v[v.size() / 2]; }
        else map.refPeriod = map.defaultPeriod;
    }
    return map;
}
}
