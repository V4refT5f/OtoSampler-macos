// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "Theme.h"

namespace oto::ui
{
// Context menu drawn by the editor itself (a child of the canvas), so it can be animated and follows the UI scale.
// Opening: fades in and springs out from the pointer, the items appear one after another.
// Hovering: one rounded highlight pill slides from item to item.
// Closing: fades out and shrinks a little. Keyboard: Up / Down / Return / Escape.
class OscMenu : public AnimatedBase
{
public:
    struct Item
    {
        int id = 0;
        juce::String text, shortcut;
        bool enabled = true, separator = false;
    };

    static Item entry (int id, const juce::String& text, const juce::String& shortcut, bool enabled)
    {
        Item it; it.id = id; it.text = text; it.shortcut = shortcut; it.enabled = enabled;
        return it;
    }
    static Item divider() { Item it; it.separator = true; return it; }

    std::function<void (int)> onPick;

    OscMenu()
    {
        setWantsKeyboardFocus (true);
        setVisible (false);
    }

    // parentBounds: area the menu must stay inside. anchor: top-left corner wanted for the menu (parent coordinates)
    void show (juce::Rectangle<int> parentBounds, juce::Point<int> anchor, std::vector<Item> list)
    {
        items = std::move (list);
        ys.clear();
        float y = (float) kPad + 8.f;
        for (const auto& it : items) { ys.push_back (y); y += it.separator ? kSepH : kItemH; }
        menuH = (int) std::lround (y - (float) kPad + 8.f);

        const int margin = 8;
        const int bx = juce::jlimit (margin, juce::jmax (margin, parentBounds.getRight() - kMenuW - margin), anchor.x);
        const int by = juce::jlimit (margin, juce::jmax (margin, parentBounds.getBottom() - menuH - margin), anchor.y);
        setBounds (bx - kPad, by - kPad, kMenuW + 2 * kPad, menuH + 2 * kPad);

        open = true; hover = -1; pressed = -1; hlA = 0.f; itemT = 0.f;
        if (animationsEnabled()) { alphaT = 0.f; sc.snap (0.88f); }
        else { alphaT = 1.f; sc.snap (1.f); itemT = 1.f; }
        setVisible (true);
        toFront (true);
        grabKeyboardFocus();
        wake();
    }

    void hide()
    {
        if (! isVisible() || ! open) return;
        open = false;
        hover = -1;
        if (! animationsEnabled()) { setVisible (false); return; }
        wake();
    }

    bool isOpen() const noexcept { return open && isVisible(); }

    bool hitTest (int x, int y) override { return menuBox().contains ((float) x, (float) y); }

    void paint (juce::Graphics& g) override
    {
        const float a = juce::jlimit (0.f, 1.f, alphaT);
        if (a < 0.003f || items.empty()) return;
        const auto& P = pal();
        const auto box = menuBox();

        g.saveState();
        g.addTransform (juce::AffineTransform::scale (juce::jmax (0.f, sc.x), juce::jmax (0.f, sc.x), box.getX(), box.getY()));

        juce::Path body;
        body.addRoundedRectangle (box, 22.f);
        juce::DropShadow (juce::Colours::black.withAlpha (0.30f * a), 14, { 0, 4 }).drawForPath (g, body);
        g.setColour (P.containerHigh.withAlpha (a));
        g.fillPath (body);
        g.setColour (P.outlineVariant.withAlpha (a));
        g.strokePath (body, juce::PathStrokeType (1.5f));

        // sliding highlight pill
        if (hlA > 0.01f)
        {
            g.setColour (P.primaryContainer.withAlpha (a * juce::jlimit (0.f, 1.f, hlA)));
            g.fillRoundedRectangle (box.getX() + 4.f, hlY.x, box.getWidth() - 8.f, kItemH - 2.f, (kItemH - 2.f) * 0.5f);
        }

        g.setFont (font (13.5f));
        for (size_t i = 0; i < items.size(); ++i)
        {
            const auto& it = items[i];
            const float t = juce::jlimit (0.f, 1.f, itemT * 1.7f - (float) i * 0.17f);
            const float e = t * t * (3.f - 2.f * t);
            const float y = ys[i] + (1.f - e) * 7.f;       // slides up while it appears

            if (it.separator)
            {
                g.setColour (P.outlineVariant.withAlpha (a * e));
                g.fillRect (box.getX() + 16.f, y + kSepH * 0.5f, box.getWidth() - 32.f, 1.f);
                continue;
            }

            const float hlNear = juce::jlimit (0.f, 1.f, 1.f - std::abs (hlY.x - (ys[i] + 1.f)) / kItemH) * juce::jlimit (0.f, 1.f, hlA);
            const auto fg = it.enabled ? P.onSurface.interpolatedWith (P.onPrimaryContainer, hlNear) : P.onSurface.withAlpha (0.38f);
            const auto tr = juce::Rectangle<float> (box.getX() + 20.f, y, box.getWidth() - 40.f, kItemH);
            g.setColour (fg.withMultipliedAlpha (a * e));
            g.setFont (font (13.5f));
            g.drawText (it.text, tr, juce::Justification::centredLeft, true);
            if (it.shortcut.isNotEmpty())
            {
                g.setColour (fg.withMultipliedAlpha (a * e * 0.65f));
                g.setFont (font (12.f));
                g.drawText (it.shortcut, tr, juce::Justification::centredRight, true);
            }
        }
        g.restoreState();
    }

    void mouseMove (const juce::MouseEvent& e) override { updateHover (e.position); }
    void mouseDrag (const juce::MouseEvent& e) override { updateHover (e.position); }
    void mouseExit (const juce::MouseEvent&) override { if (open) setHover (-1); }
    void mouseDown (const juce::MouseEvent& e) override { updateHover (e.position); pressed = hover; }
    void mouseUp (const juce::MouseEvent& e) override
    {
        const int i = itemAt (e.position);
        const int was = pressed;
        pressed = -1;
        if (open && i >= 0 && i == was) pick (i);
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (! open) return false;
        if (k == juce::KeyPress::escapeKey) { hide(); return true; }
        if (k == juce::KeyPress::downKey || k == juce::KeyPress::upKey)
        {
            const int dir = k == juce::KeyPress::downKey ? 1 : -1;
            const int n = (int) items.size();
            int i = hover;
            for (int tries = 0; tries < n; ++tries)
            {
                i = i < 0 ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
                if (selectable (i)) { setHover (i); break; }
            }
            return true;
        }
        if (k == juce::KeyPress::returnKey || k == juce::KeyPress::spaceKey)
        {
            if (hover >= 0) pick (hover);
            return true;
        }
        return false;
    }

private:
    static constexpr int kPad = 20, kMenuW = 224;       // kPad: room around the menu for the shadow and the spring overshoot
    static constexpr float kItemH = 38.f, kSepH = 13.f;

    juce::Rectangle<float> menuBox() const { return { (float) kPad, (float) kPad, (float) kMenuW, (float) menuH }; }
    bool selectable (int i) const { return i >= 0 && i < (int) items.size() && items[(size_t) i].enabled && ! items[(size_t) i].separator; }

    int itemAt (juce::Point<float> p) const
    {
        if (! menuBox().contains (p)) return -1;
        for (size_t i = 0; i < items.size(); ++i)
        {
            const float h = items[i].separator ? kSepH : kItemH;
            if (p.y >= ys[i] && p.y < ys[i] + h) return selectable ((int) i) ? (int) i : -1;
        }
        return -1;
    }
    void updateHover (juce::Point<float> p) { if (open) setHover (itemAt (p)); }
    void setHover (int i)
    {
        if (i == hover) return;
        if (i >= 0 && hlA < 0.05f) hlY.snap (ys[(size_t) i] + 1.f);   // first hover: appear in place, do not slide from an old spot
        hover = i;
        wake();
    }
    void pick (int i)
    {
        const int id = items[(size_t) i].id;
        hide();
        if (onPick) onPick (id);
    }

    bool stepAnimation (float dt) override
    {
        bool busy = false;
        const float aTarget = open ? 1.f : 0.f;
        if (animationsEnabled())
        {
            const float rate = dt / (open ? 0.10f : 0.12f);
            alphaT += juce::jlimit (-rate, rate, aTarget - alphaT);
        }
        else alphaT = aTarget;
        if (std::abs (alphaT - aTarget) > 1.0e-4f) busy = true;

        const float sTarget = open ? 1.f : 0.95f;
        sc.step (sTarget, 420.f, 24.f, dt);
        busy |= ! sc.settled (sTarget);

        if (open && itemT < 1.f)
        {
            itemT = animationsEnabled() ? juce::jmin (1.f, itemT + dt / 0.32f) : 1.f;
            busy = true;
        }

        if (hover >= 0)
        {
            const float ty = ys[(size_t) hover] + 1.f;
            hlY.step (ty, 520.f, 30.f, dt);
            busy |= ! hlY.settled (ty);
        }
        const float ht = hover >= 0 ? 1.f : 0.f;
        if (animationsEnabled()) hlA += (ht - hlA) * 0.25f; else hlA = ht;
        if (std::abs (hlA - ht) > 0.004f) busy = true; else hlA = ht;

        if (! open && alphaT <= 0.001f)
        {
            setVisible (false);
            return false;
        }
        return busy;
    }

    std::vector<Item> items;
    std::vector<float> ys;               // top of every item (component coordinates)
    int menuH = 0, hover = -1, pressed = -1;
    bool open = false;
    float alphaT = 0.f, itemT = 0.f, hlA = 0.f;
    Spring sc { 1.f, 0.f }, hlY;
};
}
