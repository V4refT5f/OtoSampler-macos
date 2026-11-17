#pragma once
#include "PluginProcessor.h"
#include "Settings.h"
#include "UI/Widgets.h"
#include "UI/WaveformView.h"
#include "UI/Panels.h"

class OtoEditor : public juce::AudioProcessorEditor,
                  public juce::FileDragAndDropTarget,
                  private juce::Timer
{
public:
    explicit OtoEditor (OtoProcessor&);
    ~OtoEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

private:
    static constexpr int kBaseW = 1180, kBaseH = 580;

    // All widgets live on a canvas that is scaled to fit the window; extra height goes to the waveform,
    // extra width is shared by the cards.
    struct Canvas : public juce::Component
    {
        std::function<void (juce::Graphics&)> onPaint;
        void paint (juce::Graphics& g) override { if (onPaint) onPaint (g); }
    };
    struct GripLookAndFeel : public juce::LookAndFeel_V4
    {
        void drawCornerResizer (juce::Graphics& g, int w, int h, bool over, bool dragging) override
        {
            const auto& P = oto::ui::pal();
            g.setColour (P.onSurfaceVariant.withAlpha (dragging ? 0.95f : (over ? 0.8f : 0.5f)));
            for (int i = 0; i < 3; ++i)
            {
                const float o = 5.f + 5.f * (float) i;
                g.drawLine ((float) w - o, (float) h - 2.f, (float) w - 2.f, (float) h - o, 2.f);
            }
        }
    };

    void layoutCanvas (int W, int H);
    void paintCanvas (juce::Graphics&);
    void timerCallback() override;
    void openFileChooser();
    void toggleTheme();
    void togglePanel();
    void saveUi();
    juce::RangedAudioParameter& P (const char* id) { return *proc.apvts.getParameter (id); }

    OtoProcessor& proc;
    Canvas canvas;
    GripLookAndFeel gripLnf;
    juce::ComponentBoundsConstrainer constrainer;

    oto::ui::WaveformView wave;
    oto::ui::PillButton openBtn { "Open Sample" };
    oto::ui::ThemeButton themeBtn;
    oto::ui::ThemeButton optionsBtn { oto::ui::ThemeButton::Icon::Options };
    oto::ui::OptionsPanel panel;

    oto::ui::Knob kRoot, kPitch, kFine, kVolume, kStart, kVel;
    oto::ui::Knob kA, kD, kS, kR;
    oto::ui::Knob kXfade, kVibRate, kVibDepth, kVibDelay, kGlide, kBend, kHp, kLp, kDenoise;
    oto::ui::ToggleSwitch swNormalize, swFormant, swSpeed, swFlatten, swLoop, swMono, swPre;
    oto::ui::FilterView filterView;

    juce::Rectangle<int> cSample, cEnv, cPlay, cLoop, cVib, cGlide, cPre;
    int lastVersion = -1;
    int startW = kBaseW, startH = kBaseH;
    std::vector<float> playheadBuf;
    juce::String shownName;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OtoEditor)
};
