// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "PluginProcessor.h"
#include "Settings.h"
#include "UI/Widgets.h"
#include "UI/OscBar.h"
#include "UI/OscMenu.h"
#include "UI/OscPage.h"
#include "UI/Panels.h"
#include "UI/VirtualKeyboard.h"

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
    bool keyPressed (const juce::KeyPress&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

private:
    static constexpr int kBaseW = 1180, kBaseH = 580 + oto::ui::kBarH;
    static constexpr int kKbH = 108, kKbGap = 8;          // virtual keyboard height and the gap above it
    int baseH() const noexcept { return kBaseH + (kbVisible ? kKbH + kKbGap : 0); }

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
    void selectOsc (int index);
    void doAdd();
    void showOscMenu (int index, juce::Point<int> posInBar);
    void pollShortcuts();
    void copyCurrent();
    void pasteCurrent();
    void duplicateCurrent();
    void deleteCurrent();
    void flashStatus (const juce::String&);
    void finishFade();
    void stepFade();
    void toggleTheme();
    void toggleKeyboard();
    void togglePanel();
    void saveUi();
    juce::RangedAudioParameter& P (const char* id) { return *proc.apvts.getParameter (id); }

    OtoProcessor& proc;
    Canvas canvas;
    GripLookAndFeel gripLnf;
    juce::ComponentBoundsConstrainer constrainer;

    std::array<std::unique_ptr<oto::ui::OscPage>, oto::kMaxOsc> pages;
    oto::ui::OscBar bar;
    oto::ui::OscMenu menu;
    oto::ui::PillButton openBtn { "Open Sample" };
    oto::ui::ThemeButton themeBtn;
    oto::ui::ThemeButton optionsBtn { oto::ui::ThemeButton::Icon::Options };
    oto::ui::ThemeButton kbBtn { oto::ui::ThemeButton::Icon::Keyboard };
    oto::ui::VirtualKeyboard keyboard;
    oto::ui::OptionsPanel panel;

    int selected = 0, lastCount = -1;
    int startW = kBaseW, startH = kBaseH;
    bool kbVisible = false;   // the virtual keyboard is always hidden at start
    juce::String shownName;
    juce::String status;                    // short message shown on the right side of the OSC bar
    juce::uint32 statusUntil = 0;
    // Keyboard shortcuts are read from the real key state as well (hosts often do not deliver key events to a plugin window)
    struct KeyWatch { int key = 0; bool down = false, pending = false; juce::uint32 edgeAt = 0; };
    std::array<KeyWatch, 4> watch;
    juce::uint32 keyHandledAt = 0;
    int heldIdx = -1;                       // shortcut whose key is still held down (blocks key repeat)
    int fadeFrom = -1;                      // page that is fading out while another one fades in
    float fadeT = 1.f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OtoEditor)
};
