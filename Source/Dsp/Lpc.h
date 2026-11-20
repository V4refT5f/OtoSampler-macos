#pragma once
#include "SampleTypes.h"

namespace oto
{
constexpr int kMaxLpcOrder = 40;
constexpr double kPreEmph = 0.9;

// Analyses the (mono) source: LPC envelope track + refined glottal pulse positions. Run once per decoded sample.
void analyzeLpc (const std::vector<float>& mono, double sampleRate, PitchMap& map, LpcTrack& lpc);

// Inverse-filters every channel of d (after Pre-filter) -> d.residual. Needs d.source->lpc.
void computeResiduals (SampleData& d);

// Reflection coefficients at (fractional) source position t
void lpcCoefAt (const LpcTrack& lpc, double t, float* out) noexcept;

// All-pole lattice synthesis filter (+ de-emphasis). Stable for |k| < 1 even when k changes every few samples.
struct LatticeSynth
{
    double b[kMaxLpcOrder + 1] {};
    double y1 = 0.0;
    void reset() noexcept { for (auto& v : b) v = 0.0; y1 = 0.0; }
    inline double process (const float* k, int order, double e) noexcept
    {
        double f = e;
        for (int m = order; m >= 1; --m)
        {
            f -= (double) k[m - 1] * b[m - 1];
            b[m] = b[m - 1] + (double) k[m - 1] * f;
        }
        b[0] = f;
        double y = f + kPreEmph * y1;
        if (! (std::abs (y) < 1.0e6)) { reset(); return 0.0; }   // safety net, never expected
        y1 = y;
        return y;
    }
};
}
