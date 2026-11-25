// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace oto
{
constexpr int kMaxPartials = 512;       // partials stored per analysis frame (upper limit)
constexpr int kMaxOutPartials = 768;    // partials a voice can play at once (more than stored when the pitch is lowered)

// Additive (harmonic + residual) model of one sample.
//
//   x(t)  =  sum_h  amp_h(t) * cos( 2*pi * ( h * theta(t) + phase_h(t) ) )   +   residual(t)
//
// theta(t) is the phase of the fundamental in cycles (a smooth, monotonic function built from the pitch track).
// Every partial is stored per frame as an amplitude and a phase offset relative to h * theta.
// The phase offset is stored as  ps_h - h * tau  (cycles):
//    tau = position of the glottal pulse inside the period, ps = the remaining "phase shape", unwrapped along h.
// ps is smooth along h, so it can be interpolated over frequency (needed to move partials while holding formants).
struct HarmonicChannel
{
    std::vector<float> amp;   // numFrames * stride, linear amplitude (0 = silent)
    std::vector<float> ps;    // numFrames * stride, phase shape in cycles
    std::vector<float> tau;   // numFrames, pulse offset in cycles of the fundamental
};

struct AdditiveData
{
    std::vector<HarmonicChannel> ch;                 // one per channel (max 2)
    std::vector<std::vector<float>> residual;        // what the partials do not explain (same rate as the source)
};

struct AdditiveModel
{
    int hop = 64;               // source samples between two frames
    int numFrames = 0;
    int stride = 0;             // partials per frame (partial h is stored at index h-1)
    std::vector<float>   f0;      // fundamental in cycles per source sample (filled through unvoiced parts)
    std::vector<double>  theta;   // phase of the fundamental in cycles
    std::vector<uint8_t> voiced;
    float refF0 = 0.f;          // median fundamental of the voiced part (target of Pitch Flatten)
    std::shared_ptr<const AdditiveData> data;   // analysis result without Pre-filter

    bool valid() const noexcept { return numFrames > 1 && stride > 0 && data != nullptr && ! data->ch.empty(); }

    // Phase of the fundamental at a (fractional) source position: cubic Hermite through theta with slope f0
    double thetaAt (double pos) const noexcept
    {
        const double fp = pos / (double) hop;
        if (fp <= 0.0) return theta[0] + (double) f0[0] * pos;
        if (fp >= (double) (numFrames - 1)) return theta[(size_t) numFrames - 1] + (double) f0[(size_t) numFrames - 1] * (pos - (double) (numFrames - 1) * hop);
        const int f = (int) fp;
        const double w = fp - (double) f, w2 = w * w, w3 = w2 * w;
        const double h = (double) hop;
        return (2 * w3 - 3 * w2 + 1) * theta[(size_t) f] + (w3 - 2 * w2 + w) * h * (double) f0[(size_t) f]
             + (-2 * w3 + 3 * w2) * theta[(size_t) f + 1] + (w3 - w2) * h * (double) f0[(size_t) f + 1];
    }

    // Fundamental (cycles per source sample) at a source position
    float f0At (double pos) const noexcept
    {
        const double fp = std::clamp (pos / (double) hop, 0.0, (double) (numFrames - 1));
        const int f = std::min ((int) fp, numFrames - 2);
        const float w = (float) (fp - (double) f);
        return f0[(size_t) f] + (f0[(size_t) f + 1] - f0[(size_t) f]) * w;
    }
};
}
