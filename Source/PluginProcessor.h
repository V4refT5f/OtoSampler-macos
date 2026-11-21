#pragma once
#include "Common.h"
#include "Params.h"
#include "Dsp/Biquad.h"
#include "Dsp/PitchAnalyzer.h"
#include "Dsp/Lpc.h"
#include "Dsp/Voice.h"

class OtoProcessor : public juce::AudioProcessor,
                     private juce::Timer,
                     private juce::AudioProcessorValueTreeState::Listener
{
public:
    OtoProcessor();
    ~OtoProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Oto Sampler"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- sample handling (message thread) ----
    void loadSampleFromFile (const juce::File&);
    std::shared_ptr<const oto::SampleData> getSampleData() const;
    juce::String getSampleName() const;
    int getSampleVersion() const noexcept { return sampleVersion.load(); }

    // MIDI note nearest to the median pitch of the voiced part of the loaded sample, or -1 when there is no sample
    // or no clear pitch (message thread)
    int detectRootNote() const;

    static constexpr int kNumVoices = 16;

    // normalised read positions (0..1) of all sounding voices, for the waveform playhead
    void getPlayheads (std::vector<float>& out) const
    {
        out.clear();
        for (auto& p : playheads) { const float v = p.load (std::memory_order_relaxed); if (v >= 0.f) out.push_back (v); }
    }

    // audition the sample (called from the UI thread, consumed by the audio thread)
    void startPreview() noexcept { ++previewOn; }
    void stopPreview() noexcept  { ++previewOff; }

    // Largest allowed Start (0..1): the loop start point while Sustain Loop is on, otherwise 1 (message thread)
    float getStartLimit() const;

    // notes played on the on-screen keyboard (also mirrors the notes arriving from the host)
    juce::MidiKeyboardState keyboardState;

    juce::AudioProcessorValueTreeState apvts;

private:
    void enforceStartLimit();
    void timerCallback() override;
    void parameterChanged (const juce::String&, float) override;

    void loadSampleFromMemory (std::shared_ptr<juce::MemoryBlock>, juce::String path, juce::String name);
    static std::shared_ptr<oto::SampleSource> decode (const juce::MemoryBlock&, const juce::String& name);
    static std::shared_ptr<oto::SampleData> buildSampleData (std::shared_ptr<const oto::SampleSource>, const oto::FilterSettings&);
    oto::FilterSettings readFilterSettings() const;
    void publish (std::shared_ptr<const oto::SampleData>, bool newSource);
    void rebuildFiltered();

    void readParams();
    void handleMidi (const juce::MidiMessage&, const oto::SampleData*);
    void noteOn (int note, float vel, const oto::SampleData*);
    void noteOff (int note);
    void renderVoices (juce::AudioBuffer<float>&, const oto::SampleData*, int start, int num);

    std::array<oto::Voice, kNumVoices> voices;
    std::array<std::atomic<float>, kNumVoices> playheads;
    oto::VoiceParams vp;
    double hostSr = 44100.0;
    std::uint64_t serialCounter = 0;
    int lastVoice = -1, lastNote = 60;
    bool hasLast = false, pedalDown = false;
    float bendNorm = 0.f, bendRangeSt = 2.f;
    bool monoMode = false;
    int seenSourceVersion = -1;
    std::atomic<unsigned> previewOn { 0 }, previewOff { 0 };
    unsigned seenPreviewOn = 0, seenPreviewOff = 0;

    // parameter pointers
    std::atomic<float> *pVolume, *pPitch, *pFine, *pRoot, *pNormalize, *pStart, *pVelocity,
        *pLoopOn, *pLoopStart, *pLoopEnd, *pLoopXfade, *pFormant, *pSpeed, *pFlatten, *pDenoise,
        *pAttack, *pDecay, *pSustain, *pRelease, *pVibRate, *pVibDepth, *pVibDelay,
        *pGlide, *pMono, *pBend, *pPreOn, *pPreHp, *pPreLp;

    // published sample (audio thread reads the raw pointer only)
    mutable juce::CriticalSection lock;
    std::shared_ptr<const oto::SampleData> owned;
    std::shared_ptr<const oto::SampleSource> currentSource;
    std::shared_ptr<juce::MemoryBlock> rawFile;
    juce::String samplePath, sampleName;
    struct Retired { std::shared_ptr<const oto::SampleData> d; juce::uint32 t; };
    std::vector<Retired> graveyard;
    std::atomic<const oto::SampleData*> audioData { nullptr };
    std::atomic<int> sampleVersion { 0 }, sourceVersion { 0 };
    std::atomic<bool> filterDirty { false }, jobRunning { false };

    juce::ThreadPool pool { 1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OtoProcessor)
};
