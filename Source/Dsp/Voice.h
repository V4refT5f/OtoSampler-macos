// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "Adsr.h"
#include "Additive.h"
#include "SampleTypes.h"
#include <array>
#include <cstdint>

namespace oto
{
// Loop range handed to the synthesis (source samples)
struct LoopInfo { bool active = false; double s = 0, e = 0, len = 0, xf = 1; };

// Per-block parameter snapshot handed to every voice
struct VoiceParams
{
    float gain = 1.f;           // linear volume
    float coarseSemis = 0.f;    // pitch + fine
    int   rootKey = 60;
    float velSens = 0.5f;
    bool  normalize = false;
    float startNorm = 0.f;
    bool  loopOn = false;
    float loopStartNorm = 0.35f, loopEndNorm = 0.65f, loopXfadeMs = 40.f;
    bool  formantHold = false;
    bool  speedHold = true;
    bool  flatten = false;      // Pitch Flattening: constant pitch (sample median) instead of the natural contour
    float denoise = 0.f;        // 0..1, used with Formant Hold and Pitch Flatten
    float attackMs = 3.f, decayMs = 300.f, sustain = 1.f, releaseMs = 120.f;
    float vibRate = 5.5f, vibDepthCents = 0.f, vibDelayMs = 0.f;
    float glideMs = 0.f;
};

// Additive voice.
// The sample is stored as partials (amplitude + phase per frame) plus a residual. Every block the voice rebuilds
//   frequency = h * (pitch of the note)      amplitude = amplitude of the partial (or of the same absolute frequency
//   phase     = h * Theta + phase offset                   when Formant Hold is on)
// for every partial and sums the oscillators. Speed Hold / varispeed only decide how fast the read position
// moves through the analysis frames; the pitch is carried by Theta, so the two are independent.
class Voice
{
public:
    void prepare (double hostSampleRate) noexcept { sr = hostSampleRate; adsr.setSampleRate (sr); kill(); }

    // glideFromPitch: absolute MIDI pitch to glide from (NaN = no glide)
    void start (const SampleData& d, int midiNote, float velocity01, float glideFromPitch,
                const VoiceParams& p, std::uint64_t serial) noexcept;
    void release() noexcept { gated = false; pedalHeld = false; adsr.noteOff(); }
    void kill() noexcept { active = false; gated = false; pedalHeld = false; adsr.kill(); }

    bool isActive() const noexcept { return active; }
    bool isGated() const noexcept { return gated; }
    int  note() const noexcept { return note_; }
    // current read position in source samples (for the UI playhead)
    double sourcePosition() const noexcept { return a; }
    float currentPitch() const noexcept { return (float) note_ + glideOffset; }
    std::uint64_t serial() const noexcept { return serial_; }

    // Adds into outL / outR (outR may be null = mono)
    void render (const SampleData& d, const VoiceParams& p, float bendSemis,
                 float* outL, float* outR, int numSamples) noexcept;

    bool pedalHeld = false;
    bool preview = false;       // started by right-click on the waveform (ignored by MIDI note-off)

private:
    // partials of one channel at one source position (loop cross-fade and Denoise included)
    void evalAt (const AdditiveModel& M, const HarmonicChannel& hc, double pos, const EvalCfg& cfg,
                 const LoopInfo& L, float denoiseW, PartialSet& out) noexcept;

    double sr = 44100.0;
    Adsr adsr;
    bool active = false, gated = false;
    int note_ = 60;
    std::uint64_t serial_ = 0;
    float velocity = 1.f;

    double a = 0.0;                  // read position in source samples
    double theta = 0.0;              // phase of the output fundamental in cycles (0..1)
    bool loopArmed = false, initDone = false;
    float bendCur = 0.f, gainSm = 0.f, glideOffset = 0.f, rPrev = 1.f;
    double lfoPhase = 0.0, vibTime = 0.0;

    // scratch (kept here so that the audio thread never allocates)
    PartialSet setA, setB, tapA, tapB, tapC;
    EvalScratch scratch;
    float pStart[kMaxOutPartials], pInc[kMaxOutPartials];
};
}
