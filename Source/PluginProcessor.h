// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "Common.h"
#include "Params.h"
#include "OscUnit.h"

class OtoProcessor : public juce::AudioProcessor,
                     private juce::Timer
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

    // ---- oscillators ----
    oto::OscUnit& osc (int i) { return *units[(size_t) juce::jlimit (0, oto::kMaxOsc - 1, i)]; }
    const oto::OscUnit& osc (int i) const { return *units[(size_t) juce::jlimit (0, oto::kMaxOsc - 1, i)]; }
    int getOscCount() const noexcept { return juce::jlimit (1, oto::kMaxOsc, (int) std::lround (pOscCount->load())); }

    // All functions below run on the message thread.
    // Add a new OSC at the end (copyFrom >= 0: start as a copy of that OSC). Returns its index, or -1 when full.
    int  addOsc (int copyFrom = -1);
    // Remove OSC "index"; the OSCs after it move up. The last remaining OSC cannot be removed.
    bool removeOsc (int index);
    // Copy the settings and the sample of an OSC into the clipboard / paste them into another OSC
    void copyOsc (int index);
    bool pasteOsc (int index);
    bool hasClipboard() const noexcept { return clip.valid; }

    // notes played on the on-screen keyboard (also mirrors the notes arriving from the host)
    juce::MidiKeyboardState keyboardState;

    juce::AudioProcessorValueTreeState apvts;

private:
    struct OscClip
    {
        bool valid = false;
        std::vector<float> norm;          // normalised value of every parameter in oto::kOscParamIds
        oto::OscUnit::SampleRef sample;
    };

    OscClip makeClip (int index) const;
    void applyClip (int index, const OscClip&);
    void resetOsc (int index);
    void setOscCount (int n);
    void timerCallback() override;

    std::atomic<float>* pOscCount = nullptr;
    OscClip clip;

    juce::ThreadPool pool { 1 };
    std::array<std::unique_ptr<oto::OscUnit>, oto::kMaxOsc> units;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OtoProcessor)
};
