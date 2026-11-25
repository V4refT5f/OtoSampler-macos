// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "AdditiveTypes.h"
#include "Biquad.h"
#include "SampleTypes.h"

namespace oto
{
// ---------------------------------------------------------------- analysis (once per loaded sample)

// Builds the additive model: pitch-adaptive harmonic analysis of every channel (amplitude + phase of up to
// kMaxPartials = 512 partials per frame) and the residual (original minus the re-synthesised partials).
// Run on a background thread; takes about a tenth of the sample length to a few seconds for long samples.
std::shared_ptr<AdditiveModel> analyzeAdditive (const std::vector<std::vector<float>>& channels, double sampleRate, const PitchMap& map);

// Applies the Pre-filter to the model: the partials get the filter response (amplitude and phase) at their own
// frequency, the residual is filtered in the time domain. Returns model.data itself when the filter is off.
std::shared_ptr<const AdditiveData> filterAdditive (const AdditiveModel& model, const FilterSettings& filter, double sampleRate);

// ---------------------------------------------------------------- synthesis helpers (shared by the analysis and the voices)

// Parameters of the partials of one channel at one source position. ph is relative to h * Theta (cycles).
struct PartialSet
{
    int n = 0;
    float amp[kMaxOutPartials];
    float ph[kMaxOutPartials];
};

struct EvalCfg
{
    bool  formantHold = false;   // false: partial h keeps its index (formants move with the pitch)
                                 // true : amplitude / phase are read at the same absolute frequency (formants stay)
    float rho = 1.f;             // output pitch / source pitch (formant hold only)
    float ampScale = 1.f;        // level compensation (formant hold only)
    int   n = 0;                 // number of partials wanted (1..n)
};

struct EvalScratch { float af[kMaxPartials + 2]; float psi[kMaxPartials + 2]; };

// Interpolates the frames around "pos" (source samples) into one set of partials
void evalPartials (const AdditiveModel& m, const HarmonicChannel& c, double pos, const EvalCfg& cfg,
                   EvalScratch& scratch, PartialSet& out) noexcept;

// a = a * (1 - w2) + b * w2, in polar form. b is first aligned to the phase of a: dFrac = theta(posA) - theta(posB) in cycles
void blendSets (PartialSet& a, const PartialSet& b, float w2, double dFrac) noexcept;

// out = wC * c + wA * a + wB * b (complex average; used by Denoise)
void averageSets (PartialSet& out, const PartialSet& c, const PartialSet& a, double dA, float wA,
                  const PartialSet& b, double dB, float wB) noexcept;

// out[i] += sum_j amp_j(i) * cos(2 pi phase_j(i)); amplitude ramps linearly a0 -> a1, phase starts at p0 (cycles, 0..1)
// and advances by inc (cycles per sample).
// Partials whose amplitude stays below "cull" at both ends of the block are skipped.
void bankAdd (const float* a0, const float* a1, const float* p0, const float* inc, int count, int len, float* out, float cull) noexcept;

constexpr int kChunk = 32;   // samples per synthesis block
}
