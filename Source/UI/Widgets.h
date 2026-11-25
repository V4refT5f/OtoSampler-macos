// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "Theme.h"

namespace oto::ui
{
// ------------------------- Knob -------------------------
class Knob : public AnimatedBase
{
public:
    static constexpr int kWidth = 80, kHeight = 106;

    Knob (juce::RangedAudioParameter& p, juce::String name, bool bipolarArc = false)
        : param (p), label (std::move (name)), bipolar (bipolarArc),
          attachment (p, [this] (float v) { normTarget = param.convertTo0to1 (v); wake(); }, nullptr)
    {
        attachment.sendInitialUpdate();
        normDisp = normTarget;
        setSize (kWidth, kHeight);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& P = pal();
        const auto b = getLocalBounds().toFloat();
        const float dial = 58.f;
        const auto dialBox = juce::Rectangle<float> (b.getX(), b.getY(), b.getWidth(), 74.f).withSizeKeepingCentre (dial, dial);
        const auto c = dialBox.getCentre();
        const float R = dial * 0.5f - 2.f, bodyR = R - 8.f;
        constexpr float a0 = -2.35619449f, span = 4.71238898f;

        g.saveState();
        g.addTransform (juce::AffineTransform::scale (scale.x, scale.x, c.x, c.y));

        juce::Path track;
        track.addCentredArc (c.x, c.y, R, R, 0.f, a0, a0 + span, true);
        g.setColour (P.outlineVariant);
        g.strokePath (track, juce::PathStrokeType (4.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const float origin = bipolar ? 0.5f : 0.f;
        const float lo = juce::jmin (origin, normDisp), hi = juce::jmax (origin, normDisp);
        if (hi - lo > 0.003f)
        {
            juce::Path v;
            v.addCentredArc (c.x, c.y, R, R, 0.f, a0 + span * lo, a0 + span * hi, true);
            g.setColour (P.primary);
            g.strokePath (v, juce::PathStrokeType (4.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        g.setColour (P.containerHigh.interpolatedWith (P.primaryContainer, hoverMix));
        g.fillEllipse (c.x - bodyR, c.y - bodyR, bodyR * 2.f, bodyR * 2.f);

        const float ang = a0 + span * normDisp;
        juce::Path ind;
        ind.startNewSubPath (c.x + std::sin (ang) * bodyR * 0.32f, c.y - std::cos (ang) * bodyR * 0.32f);
        ind.lineTo (c.x + std::sin (ang) * bodyR * 0.82f, c.y - std::cos (ang) * bodyR * 0.82f);
        g.setColour (P.onSurface.interpolatedWith (P.onPrimaryContainer, hoverMix));
        g.strokePath (ind, juce::PathStrokeType (3.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.restoreState();

        if (box == nullptr)
        {
            g.setColour (P.onSurface);
            g.setFont (font (12.5f, true));
            g.drawText (param.getCurrentValueAsText(), juce::Rectangle<int> (0, 75, getWidth(), 16), juce::Justification::centred);
        }
        g.setColour (P.onSurfaceVariant);
        g.setFont (font (11.5f));
        g.drawText (label, juce::Rectangle<int> (0, 91, getWidth(), 14), juce::Justification::centred);
    }

    // Optional limiter applied to every value this knob writes (real parameter units)
    std::function<float (float)> limitValue;

    // Optional coarse step (real parameter units). While Shift is held, dragging / wheel only lands on multiples of
    // this step (0 = disabled, Shift = fine drag).
    float shiftStep = 0.f;

    // Drag distance (pixels) for the full range of the parameter; wide ranges use a larger value to keep the
    // resolution per unit the same
    float dragPixels = 240.f;

    void mouseEnter (const juce::MouseEvent&) override { hover = true; wake(); }
    void mouseExit  (const juce::MouseEvent&) override { hover = false; wake(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())                            // right click = type a value
        {
            openTextBox();
            return;
        }
        if (e.mods.isCtrlDown() || e.mods.isCommandDown())   // Ctrl+click = reset
        {
            attachment.setValueAsCompleteGesture (param.convertFrom0to1 (param.getDefaultValue()));
            return;
        }
        pressed = true; dragStart = normTarget;
        attachment.beginGesture();
        e.source.enableUnboundedMouseMovement (true);
        wake();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! pressed) return;
        const bool stepped = shiftStep > 0.f && e.mods.isShiftDown();
        const float d = (float) (e.getDistanceFromDragStartX() - e.getDistanceFromDragStartY())
                        / ((e.mods.isShiftDown() && ! stepped) ? dragPixels * 3.75f : dragPixels);
        if (stepped) setStepped (dragStart + d);
        else         setNorm (dragStart + d, true);
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! pressed) return;
        pressed = false;
        attachment.endGesture();
        e.source.enableUnboundedMouseMovement (false);
        wake();
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override   // double click = reset to the default value
    {
        if (e.mods.isPopupMenu()) return;
        const float def = param.convertFrom0to1 (param.getDefaultValue());
        if (pressed) { attachment.setValueAsPartOfGesture (def); dragStart = normTarget; }
        else         attachment.setValueAsCompleteGesture (def);
    }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (shiftStep > 0.f && e.mods.isShiftDown())        // Shift + wheel = one step per notch
        {
            const float dy = w.deltaY != 0.f ? w.deltaY : w.deltaX;
            wheelAcc += w.isReversed ? -dy : dy;            // smooth-scrolling devices send many tiny deltas
            const int n = (int) (wheelAcc / 0.3f);
            if (n == 0) return;
            wheelAcc -= (float) n * 0.3f;
            const auto range = param.getNormalisableRange();
            const float cur = param.convertFrom0to1 (normTarget);
            float k = n > 0 ? std::floor (cur / shiftStep) + (float) n : std::ceil (cur / shiftStep) + (float) n;
            k = juce::jlimit (std::ceil (range.start / shiftStep), std::floor (range.end / shiftStep), k);
            float real = range.snapToLegalValue (k * shiftStep);
            if (limitValue) real = limitValue (real);
            attachment.setValueAsCompleteGesture (real);
            return;
        }
        setNorm (normTarget + w.deltaY * (w.isReversed ? -0.15f : 0.15f) * (240.f / dragPixels), false);
    }

private:
    void setNorm (float n, bool partOfGesture)
    {
        n = juce::jlimit (0.f, 1.f, n);
        float real = param.getNormalisableRange().snapToLegalValue (param.convertFrom0to1 (n));
        if (limitValue) real = limitValue (real);
        if (partOfGesture) attachment.setValueAsPartOfGesture (real);
        else               attachment.setValueAsCompleteGesture (real);
    }

    // Shift drag: only multiples of shiftStep, kept inside the parameter range
    void setStepped (float n)
    {
        const auto range = param.getNormalisableRange();
        const float raw = param.convertFrom0to1 (juce::jlimit (0.f, 1.f, n));
        const float lo = std::ceil (range.start / shiftStep), hi = std::floor (range.end / shiftStep);
        float k = std::round (raw / shiftStep);
        k = lo <= hi ? juce::jlimit (lo, hi, k) : k;
        float real = range.snapToLegalValue (k * shiftStep);
        if (limitValue) real = limitValue (real);
        attachment.setValueAsPartOfGesture (real);
    }

    bool stepAnimation (float dt) override
    {
        const float target = anim (pressed ? 1.07f : (hover ? 1.16f : 1.f));
        scale.step (target, 260.f, 15.f, dt);
        if (animationsEnabled())
        {
            hoverMix += ((hover ? 1.f : 0.f) - hoverMix) * 0.12f;
            normDisp += (normTarget - normDisp) * 0.18f;
        }
        else { hoverMix = hover ? 1.f : 0.f; normDisp = normTarget; }
        const bool busy = ! scale.settled (target) || std::abs (hoverMix - (hover ? 1.f : 0.f)) > 0.004f
                          || std::abs (normDisp - normTarget) > 0.0006f;
        if (! busy) { scale.snap (target); hoverMix = hover ? 1.f : 0.f; normDisp = normTarget; }
        return busy;
    }

    void openTextBox()
    {
        box = std::make_unique<juce::TextEditor>();
        const auto& P = pal();
        box->setFont (font (13.f, true));
        box->setJustification (juce::Justification::centred);
        box->setColour (juce::TextEditor::backgroundColourId, P.containerHigh);
        box->setColour (juce::TextEditor::textColourId, P.onSurface);
        box->setColour (juce::TextEditor::outlineColourId, P.primary);
        box->setColour (juce::TextEditor::focusedOutlineColourId, P.primary);
        box->setColour (juce::CaretComponent::caretColourId, P.primary);
        box->setColour (juce::TextEditor::highlightColourId, P.primaryContainer);
        box->setColour (juce::TextEditor::highlightedTextColourId, P.onPrimaryContainer);
        box->setText (param.getCurrentValueAsText(), false);
        box->setBounds (2, 72, getWidth() - 4, 20);
        addAndMakeVisible (*box);
        box->grabKeyboardFocus();
        box->selectAll();
        box->onReturnKey = [this] { finish (true); };
        box->onEscapeKey = [this] { finish (false); };
        box->onFocusLost = [this] { finish (true); };
        repaint();
    }
    void finish (bool commit)
    {
        if (box == nullptr) return;
        const auto text = box->getText();
        juce::Component::SafePointer<Knob> safe (this);
        juce::MessageManager::callAsync ([safe, text, commit]
        {
            if (safe == nullptr || safe->box == nullptr) return;
            safe->box.reset();
            if (commit)
            {
                const float n = safe->param.getValueForText (text);
                float real = safe->param.getNormalisableRange().snapToLegalValue (safe->param.convertFrom0to1 (n));
                if (safe->limitValue) real = safe->limitValue (real);
                safe->attachment.setValueAsCompleteGesture (real);
            }
            safe->repaint();
        });
    }

    juce::RangedAudioParameter& param;
    juce::String label;
    bool bipolar;
    float normTarget = 0.f, normDisp = 0.f, dragStart = 0.f, hoverMix = 0.f, wheelAcc = 0.f;
    bool hover = false, pressed = false;
    Spring scale { 1.f, 0.f };
    std::unique_ptr<juce::TextEditor> box;
    juce::ParameterAttachment attachment;
};

// ------------------------- Switch (M3 style) -------------------------
class ToggleSwitch : public AnimatedBase
{
public:
    ToggleSwitch (juce::RangedAudioParameter& p, juce::String text = {})
        : param (p), label (std::move (text)),
          attachment (p, [this] (float v) { on = v > 0.5f; wake(); }, nullptr)
    {
        attachment.sendInitialUpdate();
        thumb.snap (on ? 1.f : 0.f);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    int getIdealWidth() const { return 44 + (label.isEmpty() ? 0 : 10 + (int) std::ceil (juce::GlyphArrangement::getStringWidth (font (13.f), label))); }

    void paint (juce::Graphics& g) override
    {
        const auto& P = pal();
        const float t = juce::jlimit (0.f, 1.f, thumb.x);
        const auto tr = juce::Rectangle<float> (2.f, ((float) getHeight() - 24.f) * 0.5f, 40.f, 24.f);

        g.setColour (P.containerHigh.interpolatedWith (P.primary, t));
        g.fillRoundedRectangle (tr, 12.f);
        if (t < 0.99f)
        {
            g.setColour (P.outline.withAlpha (1.f - t));
            g.drawRoundedRectangle (tr.reduced (1.f), 11.f, 2.f);
        }
        const float cx = tr.getX() + 12.f + 16.f * thumb.x;
        const float rad = (6.5f + 2.5f * t) * juce::jmax (0.6f, grow.x);
        g.setColour (P.outline.interpolatedWith (P.onPrimary, t));
        g.fillEllipse (cx - rad, tr.getCentreY() - rad, rad * 2.f, rad * 2.f);

        if (label.isNotEmpty())
        {
            g.setColour (P.onSurface);
            g.setFont (font (13.f));
            g.drawText (label, juce::Rectangle<int> (54, 0, getWidth() - 54, getHeight()), juce::Justification::centredLeft);
        }
    }
    void mouseEnter (const juce::MouseEvent&) override { hover = true; wake(); }
    void mouseExit  (const juce::MouseEvent&) override { hover = false; pressed = false; wake(); }
    void mouseDown  (const juce::MouseEvent&) override { pressed = true; wake(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        const bool inside = getLocalBounds().contains (e.getPosition());
        pressed = false;
        if (inside) attachment.setValueAsCompleteGesture (on ? 0.f : 1.f);
        wake();
    }

private:
    bool stepAnimation (float dt) override
    {
        const float tt = on ? 1.f : 0.f, tg = anim (pressed ? 1.3f : (hover ? 1.18f : 1.f));
        thumb.step (tt, 420.f, 22.f, dt);
        grow.step (tg, 300.f, 16.f, dt);
        return ! thumb.settled (tt) || ! grow.settled (tg);
    }
    juce::RangedAudioParameter& param;
    juce::String label;
    bool on = false, hover = false, pressed = false;
    Spring thumb, grow { 1.f, 0.f };
    juce::ParameterAttachment attachment;
};

// ------------------------- Segmented selector -------------------------
class Segmented : public AnimatedBase
{
public:
    Segmented (juce::RangedAudioParameter& p, juce::StringArray names)
        : items (std::move (names)),
          attachment (p, [this] (float v) { index = juce::jlimit (0, items.size() - 1, (int) std::lround (v)); wake(); }, nullptr)
    {
        attachment.sendInitialUpdate();
        pos.snap ((float) index);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }
    void paint (juce::Graphics& g) override
    {
        const auto& P = pal();
        const auto b = getLocalBounds().toFloat();
        const float w = b.getWidth() / (float) items.size();
        g.setColour (P.outline);
        g.drawRoundedRectangle (b.reduced (0.5f), b.getHeight() * 0.5f, 1.f);
        g.setColour (P.primaryContainer);
        g.fillRoundedRectangle (juce::Rectangle<float> (pos.x * w + 3.f, 3.f, w - 6.f, b.getHeight() - 6.f), (b.getHeight() - 6.f) * 0.5f);
        g.setFont (font (12.5f, true));
        for (int i = 0; i < items.size(); ++i)
        {
            const float sel = juce::jlimit (0.f, 1.f, 1.f - std::abs (pos.x - (float) i));
            g.setColour (P.onSurfaceVariant.interpolatedWith (P.onPrimaryContainer, sel));
            g.drawText (items[i], juce::Rectangle<float> ((float) i * w, 0.f, w, b.getHeight()), juce::Justification::centred);
        }
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! getLocalBounds().contains (e.getPosition())) return;
        const int i = juce::jlimit (0, items.size() - 1, (int) ((float) e.x / ((float) getWidth() / (float) items.size())));
        attachment.setValueAsCompleteGesture ((float) i);
    }
private:
    bool stepAnimation (float dt) override { pos.step ((float) index, 380.f, 20.f, dt); return ! pos.settled ((float) index); }
    juce::StringArray items;
    int index = 0;
    Spring pos;
    juce::ParameterAttachment attachment;
};

// ------------------------- Buttons -------------------------
class PillButton : public AnimatedBase
{
public:
    explicit PillButton (juce::String t) : text (std::move (t)) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
    std::function<void()> onClick;
    void paint (juce::Graphics& g) override
    {
        const auto& P = pal();
        const auto b = getLocalBounds().toFloat().reduced (4.f);
        g.saveState();
        g.addTransform (juce::AffineTransform::scale (scale.x, scale.x, b.getCentreX(), b.getCentreY()));
        g.setColour (P.primary.interpolatedWith (P.primary.brighter (0.15f), hoverMix));
        g.fillRoundedRectangle (b, b.getHeight() * 0.5f);
        g.setColour (P.onPrimary);
        g.setFont (font (13.5f, true));
        g.drawText (text, b, juce::Justification::centred);
        g.restoreState();
    }
    void mouseEnter (const juce::MouseEvent&) override { hover = true; wake(); }
    void mouseExit  (const juce::MouseEvent&) override { hover = false; pressed = false; wake(); }
    void mouseDown  (const juce::MouseEvent&) override { pressed = true; wake(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        const bool inside = getLocalBounds().contains (e.getPosition());
        pressed = false; wake();
        if (inside && onClick) onClick();
    }
private:
    bool stepAnimation (float dt) override
    {
        const float tg = anim (pressed ? 0.95f : (hover ? 1.06f : 1.f));
        scale.step (tg, 300.f, 16.f, dt);
        if (animationsEnabled()) hoverMix += ((hover ? 1.f : 0.f) - hoverMix) * 0.15f;
        else hoverMix = hover ? 1.f : 0.f;
        return ! scale.settled (tg) || std::abs (hoverMix - (hover ? 1.f : 0.f)) > 0.004f;
    }
    juce::String text;
    bool hover = false, pressed = false;
    float hoverMix = 0.f;
    Spring scale { 1.f, 0.f };
};

// Small tonal pill for card headers. flash() shows a short message on the button for a moment.
class TonalButton : public AnimatedBase
{
public:
    explicit TonalButton (juce::String t) : text (std::move (t)), shown (text)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }
    std::function<void()> onClick;

    void flash (const juce::String& message, int ms = 1500)
    {
        shown = message; flashing = true; repaint();
        const int id = ++flashId;
        juce::Component::SafePointer<TonalButton> safe (this);
        juce::Timer::callAfterDelay (ms, [safe, id]
        {
            if (safe == nullptr || safe->flashId != id) return;
            safe->shown = safe->text; safe->flashing = false; safe->repaint();
        });
    }

    void paint (juce::Graphics& g) override
    {
        const auto& P = pal();
        const auto b = getLocalBounds().toFloat().reduced (2.f);
        g.saveState();
        g.addTransform (juce::AffineTransform::scale (scale.x, scale.x, b.getCentreX(), b.getCentreY()));
        const auto fill = flashing ? P.primaryContainer : P.containerHigh.interpolatedWith (P.primaryContainer, hoverMix);
        g.setColour (fill);
        g.fillRoundedRectangle (b, b.getHeight() * 0.5f);
        g.setColour (flashing ? P.onPrimaryContainer : P.onSurface.interpolatedWith (P.onPrimaryContainer, hoverMix));
        g.setFont (font (12.5f, true));
        g.drawText (shown, b.reduced (6.f, 0.f), juce::Justification::centred, true);
        g.restoreState();
    }
    void mouseEnter (const juce::MouseEvent&) override { hover = true; wake(); }
    void mouseExit  (const juce::MouseEvent&) override { hover = false; pressed = false; wake(); }
    void mouseDown  (const juce::MouseEvent&) override { pressed = true; wake(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        const bool inside = getLocalBounds().contains (e.getPosition());
        pressed = false; wake();
        if (inside && onClick) onClick();
    }
private:
    bool stepAnimation (float dt) override
    {
        const float tg = anim (pressed ? 0.94f : (hover ? 1.05f : 1.f));
        scale.step (tg, 300.f, 16.f, dt);
        if (animationsEnabled()) hoverMix += ((hover ? 1.f : 0.f) - hoverMix) * 0.15f;
        else hoverMix = hover ? 1.f : 0.f;
        return ! scale.settled (tg) || std::abs (hoverMix - (hover ? 1.f : 0.f)) > 0.004f;
    }
    juce::String text, shown;
    bool hover = false, pressed = false, flashing = false;
    int flashId = 0;
    float hoverMix = 0.f;
    Spring scale { 1.f, 0.f };
};

class ThemeButton : public AnimatedBase
{
public:
    enum class Icon { Theme, Options, Keyboard };
    explicit ThemeButton (Icon i = Icon::Theme) : icon (i) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
    std::function<void()> onClick;
    void setActive (bool a) { if (a != active) { active = a; repaint(); } }
    void paint (juce::Graphics& g) override
    {
        const auto& P = pal();
        const auto b = getLocalBounds().toFloat().reduced (4.f);
        const auto c = b.getCentre();
        g.saveState();
        g.addTransform (juce::AffineTransform::scale (scale.x, scale.x, c.x, c.y));
        const auto fill = (active && icon == Icon::Keyboard) ? P.primaryContainer : P.containerHigh;
        g.setColour (fill);
        g.fillEllipse (b);
        g.setColour ((active && icon == Icon::Keyboard) ? P.onPrimaryContainer : P.onSurface);
        if (icon == Icon::Keyboard)   // little piano
        {
            const auto r = juce::Rectangle<float> (c.x - 10.f, c.y - 6.5f, 20.f, 13.f);
            g.drawRoundedRectangle (r, 2.5f, 1.6f);
            for (int i = 1; i < 4; ++i) g.drawLine (r.getX() + 5.f * (float) i, r.getY() + 7.f, r.getX() + 5.f * (float) i, r.getBottom(), 1.2f);
            for (int i = 1; i < 4; ++i) g.fillRect (r.getX() + 5.f * (float) i - 1.5f, r.getY() + 1.f, 3.f, 7.5f);
        }
        else if (icon == Icon::Options)   // three sliders
        {
            const float dots[3] = { -3.f, 4.f, -1.f };
            for (int i = 0; i < 3; ++i)
            {
                const float y = c.y - 6.f + 6.f * (float) i;
                g.setColour (P.onSurface);
                g.fillRoundedRectangle (c.x - 8.f, y - 1.f, 16.f, 2.f, 1.f);
                g.setColour (P.primary);
                g.fillEllipse (c.x + dots[i] - 3.f, y - 3.f, 6.f, 6.f);
            }
        }
        else if (isDark())   // sun
        {
            g.fillEllipse (c.x - 5.f, c.y - 5.f, 10.f, 10.f);
            for (int i = 0; i < 8; ++i)
            {
                const float a = (float) i * juce::MathConstants<float>::pi / 4.f;
                g.drawLine (c.x + std::cos (a) * 8.f, c.y + std::sin (a) * 8.f, c.x + std::cos (a) * 10.5f, c.y + std::sin (a) * 10.5f, 2.f);
            }
        }
        else            // moon
        {
            g.fillEllipse (c.x - 8.f, c.y - 8.f, 16.f, 16.f);
            g.setColour (fill);
            g.fillEllipse (c.x - 1.f, c.y - 10.f, 14.f, 14.f);
        }
        g.restoreState();
    }
    void mouseEnter (const juce::MouseEvent&) override { hover = true; wake(); }
    void mouseExit  (const juce::MouseEvent&) override { hover = false; pressed = false; wake(); }
    void mouseDown  (const juce::MouseEvent&) override { pressed = true; wake(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        const bool inside = getLocalBounds().contains (e.getPosition());
        pressed = false; wake();
        if (inside && onClick) onClick();
    }
private:
    bool stepAnimation (float dt) override
    {
        const float tg = anim (pressed ? 0.92f : (hover ? 1.12f : 1.f));
        scale.step (tg, 300.f, 16.f, dt);
        return ! scale.settled (tg);
    }
    bool active = false;
    Icon icon;
    bool hover = false, pressed = false;
    Spring scale { 1.f, 0.f };
};
}
