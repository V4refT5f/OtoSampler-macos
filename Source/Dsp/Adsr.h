// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <algorithm>
#include <cmath>
namespace oto
{
class Adsr
{
public:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };

    void setSampleRate (double sr) noexcept { sampleRate = (float) sr; }
    void setParams (float attackMs, float decayMs, float sustainLevel, float releaseMs) noexcept
    {
        sustain = std::clamp (sustainLevel, 0.f, 1.f);
        attackInc   = 1.f / std::max (1.f, attackMs * 0.001f * sampleRate);
        decayCoef   = std::exp (-6.9f / std::max (1.f, decayMs * 0.001f * sampleRate));
        releaseCoef = std::exp (-6.9f / std::max (1.f, releaseMs * 0.001f * sampleRate));
    }
    void noteOn() noexcept  { stage = Stage::Attack; }
    void noteOff() noexcept { if (stage != Stage::Idle) stage = Stage::Release; }
    void kill() noexcept    { stage = Stage::Idle; level = 0.f; }
    bool isActive() const noexcept { return stage != Stage::Idle; }

    float next() noexcept
    {
        switch (stage)
        {
            case Stage::Attack:
                level += attackInc;
                if (level >= 1.f) { level = 1.f; stage = Stage::Decay; }
                break;
            case Stage::Decay:
                level = sustain + (level - sustain) * decayCoef;
                if (level - sustain <= 1e-4f)
                {
                    level = sustain;
                    stage = sustain <= 1e-4f ? Stage::Idle : Stage::Sustain;
                }
                break;
            case Stage::Sustain: level = sustain; break;
            case Stage::Release:
                level *= releaseCoef;
                if (level < 1e-4f) { level = 0.f; stage = Stage::Idle; }
                break;
            case Stage::Idle: level = 0.f; break;
        }
        return level;
    }
private:
    Stage stage = Stage::Idle;
    float level = 0.f, sustain = 1.f, attackInc = 0.01f, decayCoef = 0.999f, releaseCoef = 0.999f, sampleRate = 44100.f;
};
}
