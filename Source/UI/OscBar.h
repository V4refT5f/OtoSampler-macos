// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "Theme.h"
#include "../Params.h"

namespace oto::ui
{
// Tab strip: OSC 1 .. OSC n (n = number in use) and a "+ Add" pill while there is a free slot.
// Left click selects, right click on a tab asks the owner for a context menu.
// Every movement uses the same springs as the knobs and switches: the selection pill slides between the tabs,
// new tabs grow in, removed tabs shrink away and the Add pill glides to the next free slot.
// The Add pill also has a spinning plus icon and a ripple.
class OscBar : public AnimatedBase
{
public:
    std::function<void (int)> onSelect;
    std::function<void (int, juce::Point<int>)> onContext;   // right click on tab i (position inside this component)
    std::function<void()> onAdd;

    OscBar()
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        snapAll();
    }

    void setState (int newCount, int newSelected)
    {
        count = juce::jlimit (1, kMaxOsc, newCount);
        selected = juce::jlimit (0, count - 1, newSelected);
        if (firstState) { firstState = false; snapAll(); repaint(); }
        else wake();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& P = pal();
        g.setFont (font (13.f, true));

        auto scaled = [&] (juce::Rectangle<float> r, float sc, auto&& draw)
        {
            g.saveState();
            g.addTransform (juce::AffineTransform::scale (juce::jmax (0.f, sc), juce::jmax (0.f, sc), r.getCentreX(), r.getCentreY()));
            draw();
            g.restoreState();
        };
        auto tabScale = [&] (int i) { return tabGrow[(size_t) i].x * press[(size_t) i].x; };

        // 1. tab bodies
        for (int i = 0; i < kMaxOsc; ++i)
        {
            const float vis = juce::jlimit (0.f, 1.f, tabGrow[(size_t) i].x);
            if (vis < 0.01f) continue;
            const auto r = slotRect (i);
            scaled (r, tabScale (i), [&]
            {
                g.setColour (P.containerHigh.interpolatedWith (P.primaryContainer, 0.45f * hoverMix[(size_t) i]).withMultipliedAlpha (vis));
                g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
            });
        }

        // 2. selection pill (slides from tab to tab)
        {
            const auto r = juce::Rectangle<float> (slotX (pillX.x), slotY(), kTabW, kH);
            scaled (r, press[(size_t) selected].x, [&]
            {
                g.setColour (P.primaryContainer);
                g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
            });
        }

        // 3. tab labels
        for (int i = 0; i < kMaxOsc; ++i)
        {
            const float vis = juce::jlimit (0.f, 1.f, tabGrow[(size_t) i].x);
            if (vis < 0.01f) continue;
            const auto r = slotRect (i);
            const float sel = juce::jlimit (0.f, 1.f, 1.f - std::abs (pillX.x - (float) i));
            const float t = juce::jmax (sel, 0.6f * hoverMix[(size_t) i]);
            scaled (r, tabScale (i), [&]
            {
                g.setColour (P.onSurfaceVariant.interpolatedWith (P.onPrimaryContainer, t).withMultipliedAlpha (vis));
                g.drawText ("OSC " + juce::String (i + 1), r, juce::Justification::centred);
            });
        }

        // 4. Add pill: plus icon that spins on hover / click, ripple from the click point
        {
            const float vis = juce::jlimit (0.f, 1.f, addGrow.x);
            if (vis >= 0.01f)
            {
                const auto r = juce::Rectangle<float> (slotX (addX.x), slotY(), kAddW, kH);
                const float hm = hoverMix[(size_t) kMaxOsc];
                scaled (r, addGrow.x * press[(size_t) kMaxOsc].x, [&]
                {
                    const float rad = r.getHeight() * 0.5f;
                    g.setColour (P.container.interpolatedWith (P.primaryContainer, hm).withMultipliedAlpha (vis));
                    g.fillRoundedRectangle (r, rad);
                    g.setColour (P.outline.withMultipliedAlpha (vis * (1.f - hm)));
                    g.drawRoundedRectangle (r.reduced (0.5f), rad - 0.5f, 1.2f);

                    if (rippleP < 1.f)
                    {
                        g.saveState();
                        juce::Path clip;
                        clip.addRoundedRectangle (r, rad);
                        g.reduceClipRegion (clip, juce::AffineTransform());
                        const float e = 1.f - (1.f - rippleP) * (1.f - rippleP);   // ease out
                        const float rr = e * kAddW * 0.9f;
                        const auto c = juce::Point<float> (r.getX() + rippleC.x, r.getY() + rippleC.y);
                        g.setColour (P.primary.withAlpha (0.30f * (1.f - rippleP)));
                        g.fillEllipse (c.x - rr, c.y - rr, rr * 2.f, rr * 2.f);
                        g.restoreState();
                    }

                    const auto font13 = font (13.f, true);
                    const float textW = juce::GlyphArrangement::getStringWidth (font13, "Add");
                    const float iconW = 14.f, gap = 7.f;
                    const float x0 = r.getCentreX() - (iconW + gap + textW) * 0.5f;
                    const auto ic = juce::Point<float> (x0 + iconW * 0.5f, r.getCentreY());
                    const auto fg = P.onSurface.interpolatedWith (P.onPrimaryContainer, hm).withMultipliedAlpha (vis);
                    g.setColour (fg);
                    for (int k = 0; k < 2; ++k)
                    {
                        const float a = plusRot.x + (float) k * juce::MathConstants<float>::halfPi;
                        const float dx = std::cos (a) * 6.f, dy = std::sin (a) * 6.f;
                        juce::Path arm;
                        arm.startNewSubPath (ic.x - dx, ic.y - dy);
                        arm.lineTo (ic.x + dx, ic.y + dy);
                        g.strokePath (arm, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                    }
                    g.setFont (font13);
                    g.drawText ("Add", juce::Rectangle<float> (x0 + iconW + gap, r.getY(), textW + 4.f, r.getHeight()), juce::Justification::centredLeft);
                });
            }
        }
    }

    // only the tabs take the mouse: the empty margin must not block the waveform below
    bool hitTest (int x, int y) override { return indexAt (juce::Point<float> ((float) x, (float) y)) >= 0; }

    void mouseMove (const juce::MouseEvent& e) override { setHover (indexAt (e.position)); }
    void mouseExit (const juce::MouseEvent&) override { pressed = -1; setHover (-1); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        const int i = indexAt (e.position);
        if (i < 0) return;
        if (e.mods.isPopupMenu())
        {
            if (i < count && onContext) onContext (i, e.getPosition());
            return;
        }
        pressed = i;
        if (i >= count)
        {
            const auto r = juce::Rectangle<float> (slotX (addX.x), slotY(), kAddW, kH);
            rippleC = e.position - r.getPosition();
            rippleP = 0.f;
        }
        wake();
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        const int was = pressed;
        pressed = -1;
        wake();
        if (was < 0 || e.mods.isPopupMenu() || indexAt (e.position) != was) return;
        if (was < count) { if (onSelect) onSelect (was); }
        else if (onAdd) { ++turns; onAdd(); }
    }

private:
    static constexpr float kTabW = 84.f, kAddW = 92.f, kGap = 6.f, kH = 32.f;
public:
    static constexpr int kMargin = 10;   // = kPad, used by the owner for the component bounds
private:
    static constexpr float kPad = (float) kMargin;

    // kPad: free space around the tabs. Pills overshoot while they spring, and this keeps them from being cut off
    // (flattened) at the edge of the component. The owner makes the component kPad larger on every side.
    float slotX (float slot) const { return kPad + slot * (kTabW + kGap); }
    float slotY() const { return ((float) getHeight() - kH) * 0.5f; }
    juce::Rectangle<float> slotRect (int i) const { return { slotX ((float) i), slotY(), kTabW, kH }; }

    // hit areas follow the final layout, not the moving pills
    int indexAt (juce::Point<float> p) const
    {
        const int n = count < kMaxOsc ? count + 1 : count;
        for (int i = 0; i < n; ++i)
        {
            const auto r = i < count ? slotRect (i) : juce::Rectangle<float> (slotX ((float) i), slotY(), kAddW, kH);
            if (r.contains (p)) return i;
        }
        return -1;
    }

    void setHover (int i)
    {
        if (i == hover) return;
        hover = i;
        if (i >= 0 && i == count && count < kMaxOsc) ++turns;   // the plus icon spins a quarter turn when the pointer arrives
        wake();
    }

    void snapAll()
    {
        pillX.snap ((float) selected);
        for (int i = 0; i < kMaxOsc; ++i) tabGrow[(size_t) i].snap (i < count ? 1.f : 0.f);
        addX.snap ((float) count);
        addGrow.snap (count < kMaxOsc ? 1.f : 0.f);
        plusRot.snap ((float) turns * juce::MathConstants<float>::halfPi);
        for (auto& s : press) s.snap (1.f);
    }

    bool stepAnimation (float dt) override
    {
        bool busy = false;
        auto run = [&] (Spring& s, float target, float k, float c) { s.step (target, k, c, dt); busy |= ! s.settled (target); };

        run (pillX, (float) selected, 380.f, 20.f);
        run (addX, (float) count, 380.f, 20.f);
        run (addGrow, count < kMaxOsc ? 1.f : 0.f, 300.f, 18.f);
        run (plusRot, (float) turns * juce::MathConstants<float>::halfPi, 220.f, 13.f);
        if (rippleP < 1.f)
        {
            rippleP = animationsEnabled() ? juce::jmin (1.f, rippleP + dt / 0.5f) : 1.f;
            busy = true;
        }
        for (int i = 0; i < kMaxOsc; ++i) run (tabGrow[(size_t) i], i < count ? 1.f : 0.f, 300.f, 18.f);

        for (int i = 0; i <= kMaxOsc; ++i)
        {
            run (press[(size_t) i], anim (pressed == i ? 0.93f : 1.f), 300.f, 16.f);
            const float th = hover == i ? 1.f : 0.f;
            auto& hm = hoverMix[(size_t) i];
            if (animationsEnabled()) hm += (th - hm) * 0.15f; else hm = th;
            if (std::abs (hm - th) > 0.004f) busy = true; else hm = th;
        }
        return busy;
    }

    int count = 1, selected = 0, hover = -1, pressed = -1, turns = 0;
    bool firstState = true;
    juce::Point<float> rippleC;
    float rippleP = 1.f;
    Spring pillX, addX, addGrow, plusRot;
    std::array<Spring, kMaxOsc> tabGrow;
    std::array<Spring, kMaxOsc + 1> press;
    std::array<float, kMaxOsc + 1> hoverMix {};
};
}
