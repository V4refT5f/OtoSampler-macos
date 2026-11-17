#pragma once
#include "Theme.h"

namespace oto::ui
{
// Options popover: theme, animations, accent color (all persisted by the editor)
class OptionsPanel : public AnimatedBase
{
public:
    static constexpr int kW = 280, kH = 244;
    std::function<void (float)> onHue;     // -1 = default
    std::function<void (bool)> onDark, onAnim;
    std::function<void()> onClose;

    OptionsPanel() { setSize (kW, kH); setWantsKeyboardFocus (true); }
    void sync() { wake(); }

    void paint (juce::Graphics& g) override
    {
        const auto& P = pal();
        const auto b = getLocalBounds().toFloat().reduced (1.f);
        g.setColour (P.containerHigh);
        g.fillRoundedRectangle (b, 22.f);
        g.setColour (P.outlineVariant);
        g.drawRoundedRectangle (b, 22.f, 1.5f);

        g.setColour (P.onSurfaceVariant);
        g.setFont (font (12.5f, true));
        g.drawText ("OPTIONS", 20, 12, 200, 20, juce::Justification::centredLeft);

        drawRow (g, kRow1Y, "Dark theme", darkSw.x);
        drawRow (g, kRow2Y, "Animations", animSw.x);

        g.setColour (P.onSurfaceVariant);
        g.setFont (font (12.5f, true));
        g.drawText ("ACCENT COLOR", 20, 114, 200, 20, juce::Justification::centredLeft);

        for (int i = 0; i < kNum; ++i)
        {
            const auto c = swatchCentre (i);
            const float r = 12.f * juce::jmax (0.5f, grow[(size_t) i].x);
            g.setColour (accentSwatch (presets[i]));
            g.fillEllipse (c.x - r, c.y - r, r * 2.f, r * 2.f);
            if (isSelected (i))
            {
                g.setColour (P.onSurface);
                g.drawEllipse (c.x - 16.f, c.y - 16.f, 32.f, 32.f, 2.f);
            }
        }

        g.setColour (P.onSurfaceVariant);
        g.setFont (font (11.5f));
        g.drawText ("Custom hue", 20, 178, 200, 16, juce::Justification::centredLeft);

        const auto bar = hueBar();
        juce::ColourGradient grad (juce::Colour::fromHSL (0.f, 0.7f, 0.5f, 1.f), bar.getX(), 0.f,
                                   juce::Colour::fromHSL (1.f, 0.7f, 0.5f, 1.f), bar.getRight(), 0.f, false);
        for (int i = 1; i < 12; ++i) grad.addColour ((double) i / 12.0, juce::Colour::fromHSL ((float) i / 12.f, 0.7f, 0.5f, 1.f));
        g.setGradientFill (grad);
        g.fillRoundedRectangle (bar, bar.getHeight() * 0.5f);
        if (accentHue() >= 0.f)
        {
            const float x = bar.getX() + accentHue() * bar.getWidth();
            g.setColour (P.containerHigh);
            g.fillEllipse (x - 10.f, bar.getCentreY() - 10.f, 20.f, 20.f);
            g.setColour (accentSwatch (accentHue()));
            g.fillEllipse (x - 7.f, bar.getCentreY() - 7.f, 14.f, 14.f);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override { hoverIdx = swatchAt (e.position); wake(); }
    void mouseExit (const juce::MouseEvent&) override { hoverIdx = -1; wake(); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        const auto p = e.position;
        if (juce::Rectangle<float> (16.f, (float) kRow1Y, kW - 32.f, 32.f).contains (p)) { if (onDark) onDark (! isDark()); wake(); return; }
        if (juce::Rectangle<float> (16.f, (float) kRow2Y, kW - 32.f, 32.f).contains (p)) { if (onAnim) onAnim (! animationsEnabled()); wake(); return; }
        pick (p, true);
    }
    void mouseDrag (const juce::MouseEvent& e) override { pick (e.position, false); }
    void mouseUp (const juce::MouseEvent&) override { dragHue = false; }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey) { if (onClose) onClose(); return true; }
        return false;
    }

private:
    static constexpr int kNum = 7, kRow1Y = 38, kRow2Y = 72;
    static constexpr float presets[kNum] = { -1.f, 0.10f, 0.28f, 0.46f, 0.58f, 0.74f, 0.93f };

    void drawRow (juce::Graphics& g, int y, const juce::String& text, float t)
    {
        const auto& P = pal();
        g.setColour (P.onSurface);
        g.setFont (font (13.f));
        g.drawText (text, 20, y, 160, 32, juce::Justification::centredLeft);
        const auto tr = juce::Rectangle<float> ((float) kW - 20.f - 40.f, (float) y + 4.f, 40.f, 24.f);
        const float c = juce::jlimit (0.f, 1.f, t);
        g.setColour (P.container.interpolatedWith (P.primary, c));
        g.fillRoundedRectangle (tr, 12.f);
        if (c < 0.99f)
        {
            g.setColour (P.outline.withAlpha (1.f - c));
            g.drawRoundedRectangle (tr.reduced (1.f), 11.f, 2.f);
        }
        const float cx = tr.getX() + 12.f + 16.f * t, rad = 6.5f + 2.5f * c;
        g.setColour (P.outline.interpolatedWith (P.onPrimary, c));
        g.fillEllipse (cx - rad, tr.getCentreY() - rad, rad * 2.f, rad * 2.f);
    }

    juce::Point<float> swatchCentre (int i) const { return { 34.f + (float) i * 36.f, 152.f }; }
    juce::Rectangle<float> hueBar() const { return { 20.f, 204.f, (float) kW - 40.f, 16.f }; }
    bool isSelected (int i) const
    {
        const float h = accentHue();
        return (presets[i] < 0.f && h < 0.f) || (presets[i] >= 0.f && h >= 0.f && std::abs (h - presets[i]) < 0.004f);
    }
    int swatchAt (juce::Point<float> p) const
    {
        for (int i = 0; i < kNum; ++i) if (p.getDistanceFrom (swatchCentre (i)) <= 16.f) return i;
        return -1;
    }
    void pick (juce::Point<float> p, bool isDown)
    {
        const int s = swatchAt (p);
        if (isDown && s >= 0) { accentHue() = presets[s]; if (onHue) onHue (presets[s]); repaint(); return; }
        const auto bar = hueBar();
        if (isDown ? bar.expanded (4.f).contains (p) : dragHue)
        {
            dragHue = true;
            const float h = juce::jlimit (0.f, 1.f, (p.x - bar.getX()) / bar.getWidth());
            accentHue() = h; if (onHue) onHue (h); repaint();
        }
    }

    bool stepAnimation (float dt) override
    {
        bool busy = false;
        const float td = isDark() ? 1.f : 0.f, ta = animationsEnabled() ? 1.f : 0.f;
        darkSw.step (td, 420.f, 22.f, dt); busy |= ! darkSw.settled (td);
        animSw.step (ta, 420.f, 22.f, dt); busy |= ! animSw.settled (ta);
        for (int i = 0; i < kNum; ++i)
        {
            const float t = anim (i == hoverIdx ? 1.25f : 1.f);
            grow[(size_t) i].step (t, 300.f, 16.f, dt);
            busy |= ! grow[(size_t) i].settled (t);
        }
        return busy;
    }

    std::array<Spring, kNum> grow { Spring { 1.f, 0.f }, Spring { 1.f, 0.f }, Spring { 1.f, 0.f }, Spring { 1.f, 0.f },
                                    Spring { 1.f, 0.f }, Spring { 1.f, 0.f }, Spring { 1.f, 0.f } };
    Spring darkSw, animSw { 1.f, 0.f };
    int hoverIdx = -1;
    bool dragHue = false;
};

// Frequency response of the Pre-filter (HP + LP, 96 dB/oct each)
class FilterView : public juce::Component
{
public:
    FilterView (juce::RangedAudioParameter& on, juce::RangedAudioParameter& hp, juce::RangedAudioParameter& lp)
        : aOn (on, [this] (float v) { isOn = v > 0.5f; repaint(); }, nullptr),
          aHp (hp, [this] (float v) { fHp = v; repaint(); }, nullptr),
          aLp (lp, [this] (float v) { fLp = v; repaint(); }, nullptr)
    {
        aOn.sendInitialUpdate(); aHp.sendInitialUpdate(); aLp.sendInitialUpdate();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& P = pal();
        const auto b = getLocalBounds().toFloat();
        g.setColour (P.containerHigh);
        g.fillRoundedRectangle (b, 16.f);

        const auto in = b.reduced (12.f, 10.f).withTrimmedBottom (12.f);
        auto xOf = [&] (float f) { return in.getX() + std::log (f / 20.f) / std::log (1000.f) * in.getWidth(); };
        g.setFont (font (10.f));
        for (float f : { 100.f, 1000.f, 10000.f })
        {
            g.setColour (P.outlineVariant);
            g.fillRect (xOf (f) - 0.5f, in.getY(), 1.f, in.getHeight());
            g.setColour (P.onSurfaceVariant);
            g.drawText (f >= 1000.f ? juce::String ((int) (f / 1000.f)) + "k" : juce::String ((int) f),
                        juce::Rectangle<float> (xOf (f) - 16.f, in.getBottom() + 1.f, 32.f, 12.f), juce::Justification::centred);
        }

        juce::Path line;
        const int steps = juce::jmax (8, (int) in.getWidth() / 2);
        for (int i = 0; i <= steps; ++i)
        {
            const float t = (float) i / (float) steps;
            const float f = 20.f * std::pow (1000.f, t);
            const float lp = 1.f / (1.f + std::pow (f / fLp, 32.f));
            const float hp = 1.f / (1.f + std::pow (fHp / f, 32.f));
            const float db = juce::jlimit (-96.f, 0.f, 10.f * std::log10 (lp * hp + 1e-9f));
            const float y = in.getY() + (-db / 96.f) * (in.getHeight() - 2.f) + 1.f;
            const float x = in.getX() + t * in.getWidth();
            if (i == 0) line.startNewSubPath (x, y); else line.lineTo (x, y);
        }
        juce::Path fill (line);
        fill.lineTo (in.getRight(), in.getBottom()); fill.lineTo (in.getX(), in.getBottom()); fill.closeSubPath();
        g.setColour ((isOn ? P.primary : P.outline).withAlpha (0.18f));
        g.fillPath (fill);
        g.setColour (isOn ? P.primary : P.outline);
        g.strokePath (line, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

private:
    bool isOn = false; float fHp = 20.f, fLp = 20000.f;
    juce::ParameterAttachment aOn, aHp, aLp;
};
}
