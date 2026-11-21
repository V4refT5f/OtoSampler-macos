#pragma once
#include "Adsr.h"
#include "FormantEngine.h"
#include "SampleTypes.h"
#include <array>
#include <cstdint>

namespace oto
{
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

class Voice
{
public:
    void prepare (double hostSampleRate) noexcept { sr = hostSampleRate; adsr.setSampleRate (sr); kill(); }

    // glideFromPitch: absolute MIDI pitch to glide from (NaN = no glide)
    void start (const SampleData& d, int midiNote, float velocity01, float glideFromPitch,
                const VoiceParams& p, std::uint64_t serial) noexcept;
    void release() noexcept { gated = false; pedalHeld = false; adsr.noteOff(); }
    void kill() noexcept { active = false; gated = false; pedalHeld = false; engInited = false; adsr.kill(); }

    bool isActive() const noexcept { return active; }
    bool isGated() const noexcept { return gated; }
    int  note() const noexcept { return note_; }
    // current read position in source samples (for the UI playhead)
    double sourcePosition() const noexcept { return engInited ? eng.position() : (usingGrains ? a : pos); }
    float currentPitch() const noexcept { return (float) note_ + glideOffset; }
    std::uint64_t serial() const noexcept { return serial_; }

    // Adds into outL / outR (outR may be null = mono)
    void render (const SampleData& d, const VoiceParams& p, float bendSemis,
                 float* outL, float* outR, int numSamples) noexcept;

    bool pedalHeld = false;
    bool preview = false;       // started by right-click on the waveform (ignored by MIDI note-off)

private:
    struct Grain { bool active = false; double c1 = 0, g = 1, shift = 0, offA = 0, offB = 0, frac = 0; float gain = 1.f, w2 = 0.f, wA = 0.f, wB = 0.f; int age = 0, len = 0; };

    void spawnGrain (const SampleData& d, const VoiceParams& p, double r, double k, const LoopInfo& L) noexcept;

    double sr = 44100.0;
    Adsr adsr;
    bool active = false, gated = false;
    int note_ = 60;
    std::uint64_t serial_ = 0;
    float velocity = 1.f;

    double pos = 0.0;                // plain resampling read position
    double a = 0.0;                  // grain engine analysis position
    double spawnCountdown = 0.0;
    bool firstGrain = true, sourceDone = false, loopArmed = false, usingGrains = false;
    std::array<Grain, 48> grains {};

    float bendCur = 0.f, gainSm = 0.f, glideOffset = 0.f;
    bool initDone = false, usingGrainsPrev = false;
    double lfoPhase = 0.0, vibTime = 0.0;

    // formant-preserving engine (runs at the source rate, resampled to the host rate)
    FormantEngine eng;
    bool engInited = false, engDone = false, lpChosen = false;
    double engFrac = 0.0;
    int engTail = 0;
    float ringL[4] = {}, ringR[4] = {};
};
}
