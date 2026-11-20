#pragma once
#include "Interp.h"
#include "Lpc.h"
#include "SampleTypes.h"
#include <array>

namespace oto
{
// Loop range handed to the engines (source samples)
struct LoopInfo { bool active = false; double s = 0, e = 0, len = 0, xf = 1; };

struct EngineParams
{
    bool speedHold = true;
    bool flatten = false;
    float denoise = 0.f;
    LoopInfo loop;
};

// Formant-preserving pitch engine (LPC residual PSOLA).
// Only the excitation pulses (LPC residual) are moved in time; the vocal tract filter keeps following the
// original time line, so formants stay put and the resonances decay / overlap naturally at any pitch ratio.
// Runs at the sample rate of the source; the voice resamples its output to the host rate.
class FormantEngine
{
public:
    void start (double startPos) noexcept;

    // one output sample at the source rate; returns false once the sample has been played out completely
    bool step (const SampleData& d, const EngineParams& p, double r, float& outL, float& outR) noexcept;

    double position() const noexcept { return a; }

private:
    struct Grain
    {
        bool active = false;
        int age = 0, len = 0, nt = 0;
        double tc[4] = {}, cpos = 0.0, hl = 1.0, hr = 1.0, shift = 0.0;
        float tw[4] = {}, gain = 1.f, w2 = 0.f;
    };
    void spawn (const SampleData& d, const EngineParams& p, double r, bool loopOn) noexcept;

    double a = 0.0, nextCenter = 0.0, lastHl = 0.0, filtOff = 0.0, filtOffTarget = 0.0;
    bool sourceDone = false, loopArmed = false;
    int coefCountdown = 0, quiet = 0;
    float kA[kMaxLpcOrder] = {}, kB[kMaxLpcOrder] = {};
    LatticeSynth synL, synR;
    std::array<Grain, 64> grains {};
};
}
