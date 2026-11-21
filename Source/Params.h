#pragma once
#include "Common.h"

namespace oto::ids
{
inline constexpr const char* volume = "volume";
inline constexpr const char* pitch = "pitch";
inline constexpr const char* fine = "fine";
inline constexpr const char* root = "root";
inline constexpr const char* normalize = "normalize";
inline constexpr const char* start = "start";
inline constexpr const char* velocity = "velocity";

inline constexpr const char* loopOn = "loopOn";
inline constexpr const char* loopStart = "loopStart";
inline constexpr const char* loopEnd = "loopEnd";
inline constexpr const char* loopXfade = "loopXfade";

inline constexpr const char* formantHold = "formantHold";
inline constexpr const char* speedHold = "speedHold";
inline constexpr const char* denoise = "denoise";
inline constexpr const char* flatten = "flatten";

inline constexpr const char* attack = "attack";
inline constexpr const char* decay = "decay";
inline constexpr const char* sustain = "sustain";
inline constexpr const char* release = "release";

inline constexpr const char* vibRate = "vibRate";
inline constexpr const char* vibDepth = "vibDepth";
inline constexpr const char* vibDelay = "vibDelay";

inline constexpr const char* glide = "glide";
inline constexpr const char* mono = "mono";
inline constexpr const char* bendRange = "bendRange";

inline constexpr const char* preOn = "preOn";
inline constexpr const char* preHp = "preHp";
inline constexpr const char* preLp = "preLp";
}

namespace oto
{
inline juce::String noteName (int n)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    n = juce::jlimit (0, 127, n);
    return juce::String (names[n % 12]) + juce::String (n / 12 - 1);
}

inline int parseNoteName (const juce::String& t)
{
    auto s = t.trim();
    if (s.isEmpty()) return 60;
    const auto c = juce::CharacterFunctions::toUpperCase (s[0]);
    if (c >= 'A' && c <= 'G')
    {
        static const int base[] = { 9, 11, 0, 2, 4, 5, 7 };   // A B C D E F G
        int n = base[c - 'A'];
        int i = 1;
        if (s[i] == '#') { ++n; ++i; } else if (s[i] == 'b') { --n; ++i; }
        const int oct = s.substring (i).getIntValue();
        return juce::jlimit (0, 127, (oct + 1) * 12 + n);
    }
    return juce::jlimit (0, 127, s.getIntValue());
}

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    using PID = ParameterID;
    AudioProcessorValueTreeState::ParameterLayout layout;

    using ToText = std::function<String (float, int)>;
    using FromText = std::function<float (const String&)>;

    auto addFloat = [&] (const char* id, const char* name, NormalisableRange<float> r, float def,
                         ToText toText, FromText fromText, float centre = -1.f)
    {
        if (centre > 0.f) r.setSkewForCentre (centre);
        layout.add (std::make_unique<AudioParameterFloat> (PID { id, 1 }, name, r, def,
                    AudioParameterFloatAttributes().withStringFromValueFunction (toText).withValueFromStringFunction (fromText)));
    };
    auto addBool = [&] (const char* id, const char* name, bool def)
    {
        layout.add (std::make_unique<AudioParameterBool> (PID { id, 1 }, name, def));
    };

    auto sign = [] (float v) { return v > 0.f ? String ("+") : String(); };
    auto fmtMs = [] (float ms) { return ms < 1000.f ? String (ms, ms < 10.f ? 1 : 0) + " ms" : String (ms / 1000.f, 2) + " s"; };
    auto parseMs = [] (const String& t)
    {
        auto s = t.trim().toLowerCase(); const float v = s.getFloatValue();
        if (s.endsWith ("ms")) return v;
        if (s.endsWith ("s")) return v * 1000.f;
        return v;
    };
    auto fmtPct = [] (float v, int) { return String (v * 100.f, 1) + " %"; };
    auto parsePct = [] (const String& t) { return t.getFloatValue() / 100.f; };
    auto fmtMsT = [=] (float v, int) { return fmtMs (v); };
    auto parseMsT = [=] (const String& t) { return parseMs (t); };

    // ---- sample ----
    addFloat (ids::volume, "Volume", { -60.f, 12.f, 0.1f }, 0.f,
              [] (float v, int) { return v <= -59.9f ? String ("-inf dB") : String (v, 1) + " dB"; },
              [] (const String& t) { return t.containsIgnoreCase ("inf") ? -60.f : t.getFloatValue(); });
    addFloat (ids::pitch, "Pitch", { -48.f, 48.f, 1.f }, 0.f,
              [=] (float v, int) { return sign (v) + String (v, 0) + " st"; },
              [] (const String& t) { return t.getFloatValue(); });
    addFloat (ids::fine, "Fine", { -100.f, 100.f, 0.1f }, 0.f,
              [=] (float v, int) { return sign (v) + String (v, 1) + " ct"; },
              [] (const String& t) { return t.getFloatValue(); });
    layout.add (std::make_unique<AudioParameterInt> (PID { ids::root, 1 }, "Root Key", 0, 127, 60,
                AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int) { return noteName (v); })
                                             .withValueFromStringFunction ([] (const String& t) { return parseNoteName (t); })));
    addBool (ids::normalize, "Normalize", false);
    addFloat (ids::start, "Start", { 0.f, 1.f, 0.f }, 0.f, fmtPct, parsePct);
    addFloat (ids::velocity, "Velocity", { 0.f, 1.f, 0.01f }, 0.5f,
              [] (float v, int) { return String (juce::roundToInt (v * 100.f)) + " %"; }, parsePct);

    // ---- sustain loop ----
    addBool (ids::loopOn, "Sustain Loop", false);
    addFloat (ids::loopStart, "Loop Start", { 0.f, 1.f, 0.f }, 0.35f, fmtPct, parsePct);
    addFloat (ids::loopEnd, "Loop End", { 0.f, 1.f, 0.f }, 0.65f, fmtPct, parsePct);
    addFloat (ids::loopXfade, "Loop Xfade", { 1.f, 1000.f, 0.1f }, 40.f, fmtMsT, parseMsT, 80.f);

    // ---- time / formant (default: formant OFF, speed hold ON) ----
    addBool (ids::formantHold, "Formant Hold", false);
    addBool (ids::speedHold, "Speed Hold", true);
    addBool (ids::flatten, "Pitch Flatten", false);
    addFloat (ids::denoise, "Denoise", { 0.f, 1.f, 0.01f }, 0.f,
              [] (float v, int) { return String (juce::roundToInt (v * 100.f)) + " %"; }, parsePct);

    // ---- amp envelope ----
    addFloat (ids::attack, "Attack", { 1.f, 5000.f, 0.1f }, 3.f, fmtMsT, parseMsT, 100.f);
    addFloat (ids::decay, "Decay", { 1.f, 10000.f, 0.1f }, 300.f, fmtMsT, parseMsT, 400.f);
    addFloat (ids::sustain, "Sustain", { 0.f, 1.f, 0.001f }, 1.f,
              [] (float v, int) { return String (juce::roundToInt (v * 100.f)) + " %"; }, parsePct);
    addFloat (ids::release, "Release", { 5.f, 10000.f, 0.1f }, 120.f, fmtMsT, parseMsT, 400.f);

    // ---- vibrato ----
    addFloat (ids::vibRate, "Vib Rate", { 0.1f, 200.f, 0.01f }, 5.5f,
              [] (float v, int) { return String (v, 2) + " Hz"; },
              [] (const String& t) { return t.getFloatValue(); }, 8.f);
    addFloat (ids::vibDepth, "Vib Depth", { 0.f, 1200.f, 0.1f }, 0.f,
              [] (float v, int) { return String (v, 1) + " ct"; },
              [] (const String& t) { return t.getFloatValue(); }, 60.f);
    addFloat (ids::vibDelay, "Vib Delay", { 0.f, 3000.f, 1.f }, 0.f, fmtMsT, parseMsT, 400.f);

    // ---- glide / bend ----
    addFloat (ids::glide, "Glide", { 0.f, 2000.f, 1.f }, 0.f, fmtMsT, parseMsT, 250.f);
    addBool (ids::mono, "Mono", false);
    addFloat (ids::bendRange, "Bend Range", { 0.f, 24.f, 1.f }, 2.f,
              [] (float v, int) { return String (v, 0) + " st"; },
              [] (const String& t) { return t.getFloatValue(); });

    // ---- pre-filter ----
    addBool (ids::preOn, "Pre-filter", false);
    auto fmtHz = [] (float v, int) { return v >= 1000.f ? String (v / 1000.f, 2) + " kHz" : String (v, 0) + " Hz"; };
    auto parseHz = [] (const String& t) { auto s = t.trim().toLowerCase(); return s.contains ("k") ? s.getFloatValue() * 1000.f : s.getFloatValue(); };
    addFloat (ids::preHp, "Pre HP Cutoff", { 20.f, 20000.f, 1.f }, 20.f, fmtHz, parseHz, 600.f);
    addFloat (ids::preLp, "Pre LP Cutoff", { 20.f, 20000.f, 1.f }, 20000.f, fmtHz, parseHz, 3000.f);

    return layout;
}
}
