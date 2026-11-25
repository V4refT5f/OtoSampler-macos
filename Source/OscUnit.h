// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "Common.h"
#include "Params.h"
#include "Dsp/Biquad.h"
#include "Dsp/PitchAnalyzer.h"
#include "Dsp/Additive.h"
#include "Dsp/Voice.h"

namespace oto
{
// One oscillator: its own sample, its own parameter set (ids with the OSC suffix) and its own voices.
class OscUnit : private juce::AudioProcessorValueTreeState::Listener
{
public:
    static constexpr int kNumVoices = 16;

    // Everything needed to put the same sample into another OSC (the decoded source is shared, not copied)
    struct SampleRef
    {
        std::shared_ptr<const SampleSource> source;
        std::shared_ptr<juce::MemoryBlock> raw;
        juce::String path, name;
    };

    OscUnit (juce::AudioProcessorValueTreeState& state, int oscIndex, juce::ThreadPool& threadPool);
    ~OscUnit() override;

    int index() const noexcept { return idx; }

    // ---- message thread ----
    void loadSampleFromFile (const juce::File&);
    void loadSampleFromMemory (std::shared_ptr<juce::MemoryBlock>, juce::String path, juce::String name);
    void loadSampleShared (const SampleRef&);
    void clearSample();
    SampleRef getSampleRef() const;
    std::shared_ptr<const SampleData> getSampleData() const;
    juce::String getSampleName() const;
    int getSampleVersion() const noexcept { return sampleVersion.load(); }
    int detectRootNote() const;
    float getStartLimit() const;
    void markFilterDirty() noexcept { filterDirty = true; }
    void timerTick();

    void getPlayheads (std::vector<float>& out) const
    {
        out.clear();
        for (auto& p : playheads) { const float v = p.load (std::memory_order_relaxed); if (v >= 0.f) out.push_back (v); }
    }
    void startPreview() noexcept { ++previewOn; }
    void stopPreview() noexcept  { ++previewOff; }

    // ---- audio thread ----
    void prepare (double sampleRate);
    void setEnabled (bool);
    void beginBlock();
    void handleMidi (const juce::MidiMessage&);
    void render (juce::AudioBuffer<float>&, int start, int num);
    void endBlock();

private:
    void parameterChanged (const juce::String&, float) override { filterDirty = true; }

    static std::shared_ptr<SampleSource> decode (const juce::MemoryBlock&, const juce::String& name);
    static std::shared_ptr<SampleData> buildSampleData (std::shared_ptr<const SampleSource>, const FilterSettings&);
    FilterSettings readFilterSettings() const;
    void publish (std::shared_ptr<const SampleData>, bool newSource);
    void rebuildFiltered();
    void enforceStartLimit();

    void readParams();
    void noteOn (int note, float vel);
    void noteOff (int note);
    void killAll();

    juce::AudioProcessorValueTreeState& apvts;
    const int idx;
    juce::ThreadPool& pool;

    std::array<Voice, kNumVoices> voices;
    std::array<std::atomic<float>, kNumVoices> playheads;
    VoiceParams vp;
    const SampleData* curData = nullptr;     // sample used by the current audio block
    double hostSr = 44100.0;
    std::uint64_t serialCounter = 0;
    int lastVoice = -1, lastNote = 60;
    bool hasLast = false, pedalDown = false, enabled = true;
    float bendNorm = 0.f, bendRangeSt = 2.f;
    bool monoMode = false;
    int seenSourceVersion = -1;
    std::atomic<unsigned> previewOn { 0 }, previewOff { 0 };
    unsigned seenPreviewOn = 0, seenPreviewOff = 0;

    std::atomic<float> *pVolume, *pPitch, *pFine, *pRoot, *pNormalize, *pStart, *pVelocity,
        *pLoopOn, *pLoopStart, *pLoopEnd, *pLoopXfade, *pFormant, *pSpeed, *pFlatten, *pDenoise,
        *pAttack, *pDecay, *pSustain, *pRelease, *pVibRate, *pVibDepth, *pVibDelay,
        *pGlide, *pMono, *pBend, *pPreOn, *pPreHp, *pPreLp;

    // published sample (the audio thread reads the raw pointer only)
    mutable juce::CriticalSection lock;
    std::shared_ptr<const SampleData> owned;
    std::shared_ptr<const SampleSource> currentSource;
    std::shared_ptr<juce::MemoryBlock> rawFile;
    juce::String samplePath, sampleName;
    struct Retired { std::shared_ptr<const SampleData> d; juce::uint32 t; };
    std::vector<Retired> graveyard;
    std::atomic<const SampleData*> audioData { nullptr };
    std::atomic<int> sampleVersion { 0 }, sourceVersion { 0 };
    std::atomic<bool> filterDirty { false }, jobRunning { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OscUnit)
};
}
