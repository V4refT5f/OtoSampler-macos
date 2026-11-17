#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oto
{
// Pitch marks (pitch-synchronous anchor points) of a sample.
// Used for formant-preserving pitch shifting and for pitch-synchronous loop points.
struct PitchMap
{
    std::vector<int>     marks;    // sample positions (sorted)
    std::vector<float>   periods;  // local period in source samples
    std::vector<uint8_t> voiced;   // 1 = voiced
    float defaultPeriod = 220.0f;
    float refPeriod = 220.0f;     // median period of the voiced part (target of Pitch Flattening)

    int nearest (double pos) const noexcept
    {
        const int n = (int) marks.size();
        if (n == 0) return -1;
        const int p = pos < 0.0 ? 0 : (int) pos;
        const int i = (int) (std::lower_bound (marks.begin(), marks.end(), p) - marks.begin());
        if (i <= 0) return 0;
        if (i >= n) return n - 1;
        return (pos - marks[(size_t) i - 1] <= marks[(size_t) i] - pos) ? i - 1 : i;
    }
};

inline double snapToMark (const PitchMap& m, double pos) noexcept
{
    const int i = m.nearest (pos);
    if (i >= 0 && m.voiced[(size_t) i]) return (double) m.marks[(size_t) i];
    return pos;
}

// Loop range in source samples, snapped to pitch marks (same rule for the engine, the UI and the Start limit)
struct LoopRange
{
    double s = 0.0, e = 0.0;
    bool valid = false;
};

inline LoopRange resolveLoop (const PitchMap& m, int numSamples, float loopStartNorm, float loopEndNorm) noexcept
{
    LoopRange r;
    double ls = (double) loopStartNorm * numSamples, le = (double) loopEndNorm * numSamples;
    if (le < ls) std::swap (ls, le);
    ls = snapToMark (m, ls);
    le = snapToMark (m, le);
    if (le - ls >= 64.0) { r.s = ls; r.e = le; r.valid = true; }
    return r;
}

// Largest allowed Start (normalised 0..1) while Sustain Loop is on: the loop start point.
// Returns 1 when the loop is off or not usable (no limit).
inline float maxStartNorm (const PitchMap& m, int numSamples, bool loopOn, float loopStartNorm, float loopEndNorm) noexcept
{
    if (! loopOn || numSamples <= 0) return 1.0f;
    const auto r = resolveLoop (m, numSamples, loopStartNorm, loopEndNorm);
    if (! r.valid) return 1.0f;
    return (float) std::clamp (r.s / (double) numSamples, 0.0, 1.0);
}

// Immutable decoded sample + analysis
struct SampleSource
{
    std::vector<std::vector<float>> channels;
    double sampleRate = 44100.0;
    std::string name;
    PitchMap map;
    int numSamples() const noexcept { return channels.empty() ? 0 : (int) channels[0].size(); }
};

// What the audio thread actually reads (source after Pre-filter)
struct SampleData
{
    std::shared_ptr<const SampleSource> source;
    std::vector<std::vector<float>> channels;
    float peak = 1.0f;
    int numSamples() const noexcept { return channels.empty() ? 0 : (int) channels[0].size(); }
    int numChannels() const noexcept { return (int) channels.size(); }
};
}
