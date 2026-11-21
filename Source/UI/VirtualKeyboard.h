#pragma once
#include "Theme.h"

namespace oto::ui
{
// On-screen piano (C2..C7). Plays through the processor's MidiKeyboardState, and also shows the notes that arrive from
// the host. The key under the root note carries a small marker.
class VirtualKeyboard : public juce::Component,
                        private juce::MidiKeyboardStateListener,
                        private juce::AsyncUpdater
{
public:
    static constexpr int kFirstNote = 36, kLastNote = 96;

    explicit VirtualKeyboard (juce::MidiKeyboardState& s) : state (s) { state.addListener (this); }
    ~VirtualKeyboard() override { state.removeListener (this); cancelPendingUpdate(); }

    void setRoot (int note) { if (note != root) { root = note; repaint(); } }

    void paint (juce::Graphics& g) override
    {
        const auto& P = pal();
        g.setColour (P.container);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 22.f);

        // Flat tonal keys made from the colours the rest of the UI already uses (no bright white, no pure black)
        const bool dark = isDark();
        const auto whiteFill     = dark ? P.outlineVariant.interpolatedWith (P.onSurfaceVariant, 0.30f) : P.bg;
        const auto blackFill     = dark ? P.bg.interpolatedWith (P.container, 0.35f) : P.onSurfaceVariant;
        const auto whiteDown     = dark ? P.primary : P.primaryContainer;
        const auto blackDown     = P.primary;
        const auto whiteText     = dark ? P.onSurface : P.onSurfaceVariant;
        const auto whiteDownText = dark ? P.onPrimary : P.onPrimaryContainer;
        const auto blackDot      = dark ? P.primary : P.primaryContainer;

        for (int n = kFirstNote; n <= kLastNote; ++n)
        {
            if (isBlack (n)) continue;
            const auto r = keyRect (n).reduced (1.3f, 0.f);
            const bool down = isDown (n);
            g.setColour (down ? whiteDown : whiteFill);
            fillKey (g, r, 4.f, 9.f);

            const bool isC = n % 12 == 0;
            if (isC)
            {
                g.setColour (down ? whiteDownText : whiteText);
                g.setFont (font (10.5f, true));
                g.drawText ("C" + juce::String (n / 12 - 1), r.withTrimmedTop (r.getHeight() - 22.f).toNearestInt(),
                            juce::Justification::centred, false);
            }
            if (n == root)
            {
                g.setColour (down ? whiteDownText : P.primary);
                g.fillEllipse (r.getCentreX() - 3.5f, r.getBottom() - (isC ? 30.f : 15.f), 7.f, 7.f);
            }
        }
        for (int n = kFirstNote; n <= kLastNote; ++n)
        {
            if (! isBlack (n)) continue;
            const auto r = keyRect (n);
            const bool down = isDown (n);
            g.setColour (down ? blackDown : blackFill);
            fillKey (g, r, 2.f, 6.f);
            if (n == root)
            {
                g.setColour (down ? P.onPrimary : blackDot);
                g.fillEllipse (r.getCentreX() - 3.f, r.getBottom() - 13.f, 6.f, 6.f);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override { press (e.position); }
    void mouseDrag (const juce::MouseEvent& e) override { press (e.position); }
    void mouseUp (const juce::MouseEvent&) override { release(); }
    void visibilityChanged() override { if (! isVisible()) release(); }

private:
    static bool isBlack (int n) noexcept
    {
        static const bool b[12] = { false, true, false, true, false, false, true, false, true, false, true, false };
        return b[n % 12];
    }
    static int numWhite() noexcept
    {
        int c = 0;
        for (int n = kFirstNote; n <= kLastNote; ++n) if (! isBlack (n)) ++c;
        return c;
    }
    static int whiteIndex (int note) noexcept      // number of white keys before this note
    {
        int c = 0;
        for (int n = kFirstNote; n < note; ++n) if (! isBlack (n)) ++c;
        return c;
    }

    // key body with small rounded top corners and larger rounded bottom corners
    static void fillKey (juce::Graphics& g, juce::Rectangle<float> r, float rt, float rb)
    {
        juce::Path top, bottom;
        top.addRoundedRectangle (r.getX(), r.getY(), r.getWidth(), r.getHeight() - rb, rt, rt, true, true, false, false);
        bottom.addRoundedRectangle (r.getX(), r.getY() + rt, r.getWidth(), r.getHeight() - rt, rb, rb, false, false, true, true);
        g.fillPath (top);
        g.fillPath (bottom);
    }

    juce::Rectangle<float> keysArea() const { return getLocalBounds().toFloat().reduced (14.f); }

    juce::Rectangle<float> keyRect (int note) const
    {
        const auto a = keysArea();
        const float ww = a.getWidth() / (float) numWhite();
        const int wi = whiteIndex (note);
        if (! isBlack (note)) return { a.getX() + (float) wi * ww, a.getY(), ww, a.getHeight() };
        const float bw = ww * 0.58f;
        return { a.getX() + (float) wi * ww - bw * 0.5f, a.getY(), bw, a.getHeight() * 0.58f };
    }

    bool isDown (int note) const { return state.isNoteOnForChannels (0xffff, note); }

    int noteAt (juce::Point<float> p, float& velocity) const
    {
        const auto a = keysArea();
        p.y = juce::jlimit (a.getY(), a.getBottom() - 0.01f, p.y);
        if (p.x < a.getX() || p.x >= a.getRight()) return -1;
        velocity = juce::jlimit (0.1f, 1.f, 0.3f + 0.7f * (p.y - a.getY()) / a.getHeight());   // lower on the key = louder
        for (int n = kFirstNote; n <= kLastNote; ++n)
            if (isBlack (n) && keyRect (n).contains (p)) return n;
        for (int n = kFirstNote; n <= kLastNote; ++n)
            if (! isBlack (n) && keyRect (n).contains (p)) return n;
        return -1;
    }

    void press (juce::Point<float> p)
    {
        float vel = 0.8f;
        const int n = noteAt (p, vel);
        if (n == held) return;
        release();
        if (n >= 0) { held = n; state.noteOn (1, n, vel); }
    }
    void release()
    {
        if (held < 0) return;
        state.noteOff (1, held, 0.f);
        held = -1;
    }

    void handleNoteOn (juce::MidiKeyboardState*, int, int, float) override { triggerAsyncUpdate(); }
    void handleNoteOff (juce::MidiKeyboardState*, int, int, float) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override { repaint(); }

    juce::MidiKeyboardState& state;
    int held = -1, root = -1;
};
}
