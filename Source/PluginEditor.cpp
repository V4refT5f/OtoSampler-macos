#include "PluginEditor.h"

using namespace oto;
using namespace oto::ui;

OtoEditor::OtoEditor (OtoProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      wave (P (ids::start), P (ids::loopStart), P (ids::loopEnd), P (ids::loopOn)),
      kRoot (P (ids::root), "Root Key"), kPitch (P (ids::pitch), "Pitch", true), kFine (P (ids::fine), "Fine", true),
      kVolume (P (ids::volume), "Volume"), kStart (P (ids::start), "Start"), kVel (P (ids::velocity), "Velocity"),
      kA (P (ids::attack), "Attack"), kD (P (ids::decay), "Decay"), kS (P (ids::sustain), "Sustain"), kR (P (ids::release), "Release"),
      kXfade (P (ids::loopXfade), "Xfade"),
      kVibRate (P (ids::vibRate), "Rate"), kVibDepth (P (ids::vibDepth), "Depth"), kVibDelay (P (ids::vibDelay), "Delay"),
      kGlide (P (ids::glide), "Glide"), kBend (P (ids::bendRange), "Bend"),
      kDenoise (P (ids::denoise), "Denoise"),
      kHp (P (ids::preHp), "HP Cutoff"), kLp (P (ids::preLp), "LP Cutoff"),
      swNormalize (P (ids::normalize), "Normalize"), swFormant (P (ids::formantHold), "Formant Hold"),
      swSpeed (P (ids::speedHold), "Speed Hold"), swFlatten (P (ids::flatten), "Pitch Flatten"), swLoop (P (ids::loopOn)), swMono (P (ids::mono), "Mono"),
      swPre (P (ids::preOn)),
      filterView (P (ids::preOn), P (ids::preHp), P (ids::preLp))
{
    {
        const auto ui = oto::loadUiSettings();
        oto::ui::isDark() = ui.dark;
        oto::ui::accentHue() = ui.hue;
        oto::ui::animationsEnabled() = ui.animations;
        startW = ui.width; startH = ui.height;
    }

    addAndMakeVisible (canvas);
    canvas.onPaint = [this] (juce::Graphics& g) { paintCanvas (g); };

    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &wave, &openBtn, &optionsBtn, &themeBtn,
             &kRoot, &kPitch, &kFine, &kVolume, &kStart, &kVel, &kA, &kD, &kS, &kR,
             &kXfade, &kVibRate, &kVibDepth, &kVibDelay, &kGlide, &kBend, &kDenoise, &kHp, &kLp,
             &swNormalize, &swFormant, &swSpeed, &swFlatten, &swLoop, &swMono, &swPre, &filterView })
        canvas.addAndMakeVisible (c);
    canvas.addChildComponent (panel);

    panel.onHue = [this] (float h) { oto::ui::accentHue() = h; saveUi(); repaint(); };
    panel.onDark = [this] (bool d) { oto::ui::isDark() = d; saveUi(); repaint(); };
    panel.onAnim = [this] (bool a) { oto::ui::animationsEnabled() = a; saveUi(); repaint(); };
    panel.sync();
    panel.onClose = [this] { panel.setVisible (false); };
    optionsBtn.onClick = [this] { togglePanel(); };
    juce::Desktop::getInstance().addGlobalMouseListener (this);

    kStart.limitValue = [this] (float v) { return juce::jmin (v, proc.getStartLimit()); };

    openBtn.onClick = [this] { openFileChooser(); };
    wave.onClickEmpty = [this] { openFileChooser(); };
    {
        auto* pp = &proc;
        wave.onPreview = [pp] (bool on) { if (on) pp->startPreview(); else pp->stopPreview(); };
    }
    themeBtn.onClick = [this] { toggleTheme(); };

    setLookAndFeel (&gripLnf);
    constrainer.setMinimumSize (720, 360);
    constrainer.setMaximumSize (3840, 2160);
    setConstrainer (&constrainer);
    setResizable (true, true);             // drag the bottom-right corner to resize
    setSize (startW, startH);
    startTimerHz (60);
}

OtoEditor::~OtoEditor()
{
    juce::Desktop::getInstance().removeGlobalMouseListener (this);
    stopTimer();
    saveUi();                              // remembers the window size
    setLookAndFeel (nullptr);
    setConstrainer (nullptr);
}

void OtoEditor::saveUi()
{
    oto::saveUiSettings ({ oto::ui::isDark(), oto::ui::accentHue(), oto::ui::animationsEnabled(), getWidth(), getHeight() });
}

void OtoEditor::togglePanel()
{
    if (panel.isVisible()) { panel.setVisible (false); return; }
    panel.setTopLeftPosition (canvas.getWidth() - 16 - panel.getWidth(), 60);
    panel.setVisible (true);
    panel.toFront (true);
}

void OtoEditor::mouseDown (const juce::MouseEvent& e)
{
    if (! panel.isVisible()) return;
    auto* c = e.eventComponent;
    if (c == &panel || panel.isParentOf (c) || c == &optionsBtn) return;
    panel.setVisible (false);
}

void OtoEditor::timerCallback()
{
    proc.getPlayheads (playheadBuf);
    wave.setPlayheads (playheadBuf);

    const int v = proc.getSampleVersion();
    if (v != lastVersion)
    {
        lastVersion = v;
        wave.setData (proc.getSampleData(), proc.getSampleName());
        shownName = proc.getSampleName();
        canvas.repaint (0, 0, canvas.getWidth(), 64);
    }
}

void OtoEditor::toggleTheme()
{
    oto::ui::isDark() = ! oto::ui::isDark();
    saveUi();
    panel.sync();
    repaint();
}

void OtoEditor::openFileChooser()
{
    chooser = std::make_unique<juce::FileChooser> ("Select a sample", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f.existsAsFile()) proc.loadSampleFromFile (f);
                          });
}

bool OtoEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (juce::File (f).hasFileExtension ("wav;aif;aiff;flac;mp3;ogg")) return true;
    return false;
}

void OtoEditor::filesDropped (const juce::StringArray& files, int, int)
{
    for (auto& f : files)
        if (juce::File (f).hasFileExtension ("wav;aif;aiff;flac;mp3;ogg")) { proc.loadSampleFromFile (juce::File (f)); break; }
}

void OtoEditor::resized()
{
    const float W = (float) getWidth(), H = (float) getHeight();
    const float sc = juce::jmax (0.1f, juce::jmin (W / (float) kBaseW, H / (float) kBaseH));
    const int vw = (int) std::ceil (W / sc), vh = (int) std::ceil (H / sc);
    canvas.setTransform (juce::AffineTransform::scale (sc));
    canvas.setBounds (0, 0, vw, vh);
    layoutCanvas (vw, vh);
}

void OtoEditor::layoutCanvas (int W, int H)
{
    const int M = 16, G = 8;
    const int extra = juce::jmax (0, H - kBaseH);
    themeBtn.setBounds (W - M - 44, 12, 44, 44);
    optionsBtn.setBounds (W - M - 44 - 44, 12, 44, 44);
    openBtn.setBounds (W - M - 44 - 44 - 8 - 132, 12, 132, 44);
    wave.setBounds (M, 64, W - 2 * M, 176 + extra);

    const int rowA = 248 + extra, rowB = 408 + extra, rowH = 152;
    const int kw = Knob::kWidth;
    const int avail = W - 2 * M;

    auto split = [&] (int y, const std::vector<int>& base, const std::vector<juce::Rectangle<int>*>& out)
    {
        int sum = 0;
        for (int b : base) sum += b;
        const int usable = avail - G * ((int) base.size() - 1);
        int x = M;
        for (size_t i = 0; i < base.size(); ++i)
        {
            const int w = (i + 1 == base.size()) ? (M + avail - x) : (int) std::lround ((double) base[i] * usable / sum);
            *out[i] = { x, y, w, rowH };
            x += w + G;
        }
    };
    split (rowA, { 504, 344, 284 }, { &cSample, &cEnv, &cPlay });
    split (rowB, { 170, 264, 184, 506 }, { &cLoop, &cVib, &cGlide, &cPre });

    auto placeKnobs = [&] (juce::Rectangle<int> card, std::initializer_list<Knob*> ks)
    {
        const int n = (int) ks.size();
        const int cell = (card.getWidth() - 24) / n;
        int i = 0;
        for (auto* k : ks) { k->setTopLeftPosition (card.getX() + 12 + i * cell + (cell - kw) / 2, card.getY() + 38); ++i; }
    };
    placeKnobs (cSample, { &kRoot, &kPitch, &kFine, &kVolume, &kStart, &kVel });
    placeKnobs (cEnv, { &kA, &kD, &kS, &kR });
    placeKnobs (cLoop, { &kXfade });
    placeKnobs (cVib, { &kVibRate, &kVibDepth, &kVibDelay });
    placeKnobs (cGlide, { &kGlide, &kBend });

    // playback switches
    int y = cPlay.getY() + 40;
    for (auto* s : { &swFormant, &swSpeed, &swFlatten })
    {
        s->setBounds (cPlay.getX() + 16, y, 150, 32);
        y += 36;
    }
    kDenoise.setTopLeftPosition (cPlay.getRight() - 12 - Knob::kWidth, cPlay.getY() + 38);

    // header switches
    swNormalize.setBounds (cSample.getRight() - 12 - swNormalize.getIdealWidth(), cSample.getY() + 8, swNormalize.getIdealWidth(), 28);
    swLoop.setBounds (cLoop.getRight() - 12 - 44, cLoop.getY() + 8, 44, 28);
    swMono.setBounds (cGlide.getRight() - 12 - swMono.getIdealWidth(), cGlide.getY() + 8, swMono.getIdealWidth(), 28);
    swPre.setBounds (cPre.getRight() - 12 - 44, cPre.getY() + 8, 44, 28);

    // pre-filter content
    kHp.setTopLeftPosition (cPre.getX() + 12, cPre.getY() + 38);
    kLp.setTopLeftPosition (kHp.getRight(), cPre.getY() + 38);
    filterView.setBounds (kLp.getRight() + 8, cPre.getY() + 42, cPre.getRight() - 14 - (kLp.getRight() + 8), 96);
}

void OtoEditor::paint (juce::Graphics& g)
{
    g.fillAll (oto::ui::pal().bg);
}

void OtoEditor::paintCanvas (juce::Graphics& g)
{
    const auto& Pl = oto::ui::pal();
    g.fillAll (Pl.bg);

    // header: badge + title + sample name
    const auto badge = juce::Rectangle<float> (18.f, 14.f, 40.f, 40.f);
    g.setColour (Pl.primaryContainer);
    g.fillEllipse (badge);
    g.setColour (Pl.onPrimaryContainer);
    {
        const float hs[5] = { 8.f, 16.f, 22.f, 13.f, 6.f };
        for (int i = 0; i < 5; ++i)
            g.fillRoundedRectangle (badge.getX() + 8.f + 5.5f * (float) i, badge.getCentreY() - hs[i] * 0.5f, 3.2f, hs[i], 1.6f);
    }

    g.setColour (Pl.onSurface);
    g.setFont (font (25.f, true));
    g.drawText ("Oto Sampler", juce::Rectangle<int> (68, 10, 220, 48), juce::Justification::centredLeft);

    const int nameX = 290, nameR = openBtn.getX() - 12;
    g.setColour (Pl.onSurfaceVariant);
    g.setFont (font (13.5f));
    g.drawText (shownName.isEmpty() ? juce::String ("No sample loaded") : shownName,
                juce::Rectangle<int> (nameX, 10, juce::jmax (10, nameR - nameX), 48), juce::Justification::centredRight, true);

    auto card = [&] (juce::Rectangle<int> r, const juce::String& title)
    {
        g.setColour (Pl.container);
        g.fillRoundedRectangle (r.toFloat(), 22.f);
        g.setColour (Pl.onSurfaceVariant);
        g.setFont (font (12.5f, true));
        g.drawText (title.toUpperCase(), r.getX() + 18, r.getY() + 12, r.getWidth() - 100, 20, juce::Justification::centredLeft);
    };
    card (cSample, "Sample");
    card (cEnv, "Amp Envelope");
    card (cPlay, "Playback");
    card (cLoop, "Sustain Loop");
    card (cVib, "Vibrato");
    card (cGlide, "Glide");
    card (cPre, "Pre-filter");
}
