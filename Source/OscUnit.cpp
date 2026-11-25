// SPDX-License-Identifier: AGPL-3.0-only
#include "OscUnit.h"

namespace oto
{
OscUnit::OscUnit (juce::AudioProcessorValueTreeState& s, int index, juce::ThreadPool& tp)
    : apvts (s), idx (index), pool (tp)
{
    auto get = [this] (const char* id) { return apvts.getRawParameterValue (oscParamId (id, idx)); };
    pVolume = get (ids::volume); pPitch = get (ids::pitch); pFine = get (ids::fine); pRoot = get (ids::root);
    pNormalize = get (ids::normalize); pStart = get (ids::start); pVelocity = get (ids::velocity);
    pLoopOn = get (ids::loopOn); pLoopStart = get (ids::loopStart); pLoopEnd = get (ids::loopEnd); pLoopXfade = get (ids::loopXfade);
    pFormant = get (ids::formantHold); pSpeed = get (ids::speedHold); pFlatten = get (ids::flatten); pDenoise = get (ids::denoise);
    pAttack = get (ids::attack); pDecay = get (ids::decay); pSustain = get (ids::sustain); pRelease = get (ids::release);
    pVibRate = get (ids::vibRate); pVibDepth = get (ids::vibDepth); pVibDelay = get (ids::vibDelay);
    pGlide = get (ids::glide); pMono = get (ids::mono); pBend = get (ids::bendRange);
    pPreOn = get (ids::preOn); pPreHp = get (ids::preHp); pPreLp = get (ids::preLp);

    for (auto* id : { ids::preOn, ids::preHp, ids::preLp })
        apvts.addParameterListener (oscParamId (id, idx), this);

    for (auto& p : playheads) p.store (-1.f);
}

OscUnit::~OscUnit()
{
    for (auto* id : { ids::preOn, ids::preHp, ids::preLp })
        apvts.removeParameterListener (oscParamId (id, idx), this);
}

void OscUnit::prepare (double sr)
{
    hostSr = sr;
    for (auto& v : voices) v.prepare (sr);
    hasLast = false;
}

void OscUnit::killAll()
{
    for (auto& v : voices) v.kill();
    for (auto& p : playheads) p.store (-1.f, std::memory_order_relaxed);
    hasLast = false;
    pedalDown = false;
}

void OscUnit::setEnabled (bool e)
{
    if (e == enabled) return;
    enabled = e;
    killAll();
    seenPreviewOn = previewOn.load();
    seenPreviewOff = previewOff.load();
}

// ======================= sample loading =======================
std::shared_ptr<SampleSource> OscUnit::decode (const juce::MemoryBlock& mb, const juce::String& name)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (
        fm.createReaderFor (std::make_unique<juce::MemoryInputStream> (mb.getData(), mb.getSize(), false)));
    if (reader == nullptr || reader->lengthInSamples <= 0) return nullptr;

    const int numCh = juce::jmin (2, (int) reader->numChannels);
    const int len = (int) juce::jmin<juce::int64> (reader->lengthInSamples, 12000000);
    juce::AudioBuffer<float> tmp (numCh, len);
    reader->read (&tmp, 0, len, 0, true, numCh > 1);

    auto src = std::make_shared<SampleSource>();
    src->sampleRate = reader->sampleRate > 0 ? reader->sampleRate : 44100.0;
    src->name = name.toStdString();
    src->channels.resize ((size_t) numCh);
    for (int c = 0; c < numCh; ++c)
        src->channels[(size_t) c].assign (tmp.getReadPointer (c), tmp.getReadPointer (c) + len);

    // tiny fades at both ends (no clicks on start / end of sample)
    const int fade = juce::jmin (64, len / 4);
    for (auto& ch : src->channels)
        for (int i = 0; i < fade; ++i)
        {
            const float g = (float) i / (float) fade;
            ch[(size_t) i] *= g;
            ch[(size_t) (len - 1 - i)] *= g;
        }

    std::vector<float> mono ((size_t) len);
    for (int i = 0; i < len; ++i)
    {
        float s = 0.f;
        for (auto& ch : src->channels) s += ch[(size_t) i];
        mono[(size_t) i] = s / (float) numCh;
    }
    src->map = analyzePitch (mono, src->sampleRate);
    // additive model: partials (amplitude + phase per frame) and residual; the sample is played back from this data
    if (auto model = analyzeAdditive (src->channels, src->sampleRate, src->map)) src->model = *model;
    return src;
}

std::shared_ptr<SampleData> OscUnit::buildSampleData (std::shared_ptr<const SampleSource> src, const FilterSettings& f)
{
    auto d = std::make_shared<SampleData>();
    d->source = src;
    d->channels = src->channels;
    float peak = 0.f;
    for (auto& ch : d->channels)
    {
        applyPreFilter (ch, src->sampleRate, f);
        for (float v : ch) peak = std::max (peak, std::abs (v));
    }
    d->peak = std::max (peak, 1e-6f);
    d->add = filterAdditive (src->model, f, src->sampleRate);   // Pre-filter applied to the partials (response) and the residual
    return d;
}

FilterSettings OscUnit::readFilterSettings() const
{
    FilterSettings f;
    f.on = pPreOn->load() > 0.5f;
    f.hp = pPreHp->load();
    f.lp = pPreLp->load();
    return f;
}

void OscUnit::publish (std::shared_ptr<const SampleData> d, bool newSource)
{
    {
        const juce::ScopedLock sl (lock);
        if (owned) graveyard.push_back ({ owned, juce::Time::getMillisecondCounter() });
        owned = d;
        audioData.store (d.get(), std::memory_order_release);
    }
    if (newSource) ++sourceVersion;
    ++sampleVersion;
}

void OscUnit::loadSampleFromFile (const juce::File& f)
{
    auto mb = std::make_shared<juce::MemoryBlock>();
    if (! f.loadFileAsData (*mb)) return;
    loadSampleFromMemory (mb, f.getFullPathName(), f.getFileName());
}

void OscUnit::loadSampleFromMemory (std::shared_ptr<juce::MemoryBlock> mb, juce::String path, juce::String name)
{
    pool.addJob ([this, mb, path, name]
    {
        auto src = decode (*mb, name);
        if (src == nullptr) return;
        {
            const juce::ScopedLock sl (lock);
            currentSource = src; rawFile = mb; samplePath = path; sampleName = name;
        }
        publish (buildSampleData (src, readFilterSettings()), true);
    });
}

// Same sample as another OSC (copy / paste): the decoded and analysed source is shared, only the Pre-filter pass is redone
void OscUnit::loadSampleShared (const SampleRef& r)
{
    if (r.source == nullptr) { clearSample(); return; }
    pool.addJob ([this, r]
    {
        {
            const juce::ScopedLock sl (lock);
            currentSource = r.source; rawFile = r.raw; samplePath = r.path; sampleName = r.name;
        }
        publish (buildSampleData (r.source, readFilterSettings()), true);
    });
}

void OscUnit::clearSample()
{
    pool.addJob ([this]
    {
        {
            const juce::ScopedLock sl (lock);
            currentSource.reset(); rawFile.reset(); samplePath = {}; sampleName = {};
        }
        publish (nullptr, true);
    });
}

OscUnit::SampleRef OscUnit::getSampleRef() const
{
    const juce::ScopedLock sl (lock);
    return { currentSource, rawFile, samplePath, sampleName };
}

void OscUnit::rebuildFiltered()
{
    std::shared_ptr<const SampleSource> src;
    { const juce::ScopedLock sl (lock); src = currentSource; }
    if (src != nullptr) publish (buildSampleData (src, readFilterSettings()), false);
}

std::shared_ptr<const SampleData> OscUnit::getSampleData() const
{
    const juce::ScopedLock sl (lock);
    return owned;
}

int OscUnit::detectRootNote() const
{
    const auto d = getSampleData();
    if (d == nullptr || d->source == nullptr) return -1;
    const auto& m = d->source->map;
    if (m.voiced.size() != m.marks.size() || m.periods.size() != m.marks.size()) return -1;

    std::vector<float> per;
    for (size_t i = 0; i < m.marks.size(); ++i)
        if (m.voiced[i] && m.periods[i] > 1.f) per.push_back (m.periods[i]);
    if (per.size() < 6) return -1;                                   // not enough pitched material

    std::nth_element (per.begin(), per.begin() + (long) (per.size() / 2), per.end());
    const double hz = d->source->sampleRate / (double) per[per.size() / 2];
    if (! (hz > 16.0 && hz < 8000.0)) return -1;

    const int note = (int) std::lround (69.0 + 12.0 * std::log2 (hz / 440.0));
    return juce::jlimit (0, 127, note);
}

juce::String OscUnit::getSampleName() const
{
    const juce::ScopedLock sl (lock);
    return sampleName;
}

float OscUnit::getStartLimit() const
{
    const auto d = getSampleData();
    if (d == nullptr || d->source == nullptr) return 1.0f;
    return maxStartNorm (d->source->map, d->numSamples(), pLoopOn->load() > 0.5f, pLoopStart->load(), pLoopEnd->load());
}

// Start is pulled back whenever it is beyond the loop start point (loop marker moved, loop switched on, ...)
void OscUnit::enforceStartLimit()
{
    const float lim = getStartLimit();
    auto* ps = apvts.getParameter (oscParamId (ids::start, idx));
    if (ps != nullptr && ps->getValue() > lim + 1.0e-6f)
    {
        ps->beginChangeGesture();
        ps->setValueNotifyingHost (lim);
        ps->endChangeGesture();
    }
}

void OscUnit::timerTick()
{
    enforceStartLimit();
    {
        const juce::ScopedLock sl (lock);
        const auto now = juce::Time::getMillisecondCounter();
        graveyard.erase (std::remove_if (graveyard.begin(), graveyard.end(),
                         [now] (const Retired& r) { return now - r.t > 3000; }), graveyard.end());
    }
    if (filterDirty.load() && ! jobRunning.load())
    {
        filterDirty = false;
        jobRunning = true;
        pool.addJob ([this] { rebuildFiltered(); jobRunning = false; });
    }
}

// ======================= audio =======================
void OscUnit::readParams()
{
    vp.gain = juce::Decibels::decibelsToGain (pVolume->load(), -60.f);
    vp.coarseSemis = pPitch->load() + pFine->load() * 0.01f;
    vp.rootKey = (int) pRoot->load();
    vp.velSens = pVelocity->load();
    vp.normalize = pNormalize->load() > 0.5f;
    vp.startNorm = pStart->load();
    vp.loopOn = pLoopOn->load() > 0.5f;
    vp.loopStartNorm = pLoopStart->load();
    vp.loopEndNorm = pLoopEnd->load();
    vp.loopXfadeMs = pLoopXfade->load();
    vp.formantHold = pFormant->load() > 0.5f;
    vp.speedHold = pSpeed->load() > 0.5f;
    vp.flatten = pFlatten->load() > 0.5f;
    vp.denoise = pDenoise->load();
    vp.attackMs = pAttack->load(); vp.decayMs = pDecay->load();
    vp.sustain = pSustain->load(); vp.releaseMs = pRelease->load();
    vp.vibRate = pVibRate->load(); vp.vibDepthCents = pVibDepth->load(); vp.vibDelayMs = pVibDelay->load();
    vp.glideMs = pGlide->load();
    monoMode = pMono->load() > 0.5f;
    bendRangeSt = pBend->load();
}

void OscUnit::noteOn (int note, float vel)
{
    const SampleData* d = curData;
    if (d == nullptr) return;
    float glideFrom = std::numeric_limits<float>::quiet_NaN();
    if (hasLast)
        glideFrom = (lastVoice >= 0 && voices[(size_t) lastVoice].isActive()) ? voices[(size_t) lastVoice].currentPitch()
                                                                               : (float) lastNote;
    if (monoMode)
        for (auto& v : voices) if (v.isActive() && v.isGated()) v.release();

    int slot = -1;
    for (int i = 0; i < kNumVoices; ++i) if (! voices[(size_t) i].isActive()) { slot = i; break; }
    if (slot < 0)   // steal: oldest releasing voice, else oldest voice
    {
        std::uint64_t best = ~0ull; bool bestGated = true;
        for (int i = 0; i < kNumVoices; ++i)
        {
            const auto& v = voices[(size_t) i];
            if ((bestGated && ! v.isGated()) || (v.isGated() == bestGated && v.serial() < best))
            { slot = i; best = v.serial(); bestGated = v.isGated(); }
        }
    }
    voices[(size_t) slot].start (*d, note, vel, glideFrom, vp, ++serialCounter);
    lastVoice = slot; lastNote = note; hasLast = true;
}

void OscUnit::noteOff (int note)
{
    for (auto& v : voices)
        if (v.isActive() && v.isGated() && ! v.preview && v.note() == note)
        {
            if (pedalDown) v.pedalHeld = true; else v.release();
        }
}

void OscUnit::handleMidi (const juce::MidiMessage& m)
{
    if (m.isNoteOn())            noteOn (m.getNoteNumber(), m.getFloatVelocity());
    else if (m.isNoteOff())      noteOff (m.getNoteNumber());
    else if (m.isPitchWheel())   bendNorm = (float) (m.getPitchWheelValue() - 8192) / 8192.f;
    else if (m.isController())
    {
        const int cc = m.getControllerNumber();
        if (cc == 64)
        {
            pedalDown = m.getControllerValue() >= 64;
            if (! pedalDown) for (auto& v : voices) if (v.isActive() && v.pedalHeld) v.release();
        }
        else if (cc == 123) for (auto& v : voices) v.release();
        else if (cc == 120) for (auto& v : voices) v.kill();
    }
    else if (m.isAllNotesOff()) for (auto& v : voices) v.release();
    else if (m.isAllSoundOff()) for (auto& v : voices) v.kill();
}

void OscUnit::beginBlock()
{
    readParams();
    curData = audioData.load (std::memory_order_acquire);

    const int sv = sourceVersion.load();
    if (sv != seenSourceVersion) { for (auto& v : voices) v.kill(); seenSourceVersion = sv; hasLast = false; }

    const unsigned pOn = previewOn.load(), pOff = previewOff.load();
    if (pOn != seenPreviewOn)
    {
        seenPreviewOn = pOn;
        if (curData != nullptr)
        {
            noteOn ((int) pRoot->load(), 0.8f);
            if (lastVoice >= 0) voices[(size_t) lastVoice].preview = true;
        }
    }
    if (pOff != seenPreviewOff)
    {
        seenPreviewOff = pOff;
        for (auto& v : voices) if (v.preview && v.isActive() && v.isGated()) v.release();
    }
}

void OscUnit::render (juce::AudioBuffer<float>& buf, int start, int num)
{
    const SampleData* d = curData;
    if (d == nullptr || num <= 0) return;
    float* L = buf.getWritePointer (0) + start;
    float* R = buf.getNumChannels() > 1 ? buf.getWritePointer (1) + start : nullptr;
    const float bend = bendNorm * bendRangeSt;
    for (auto& v : voices)
        if (v.isActive()) v.render (*d, vp, bend, L, R, num);
}

void OscUnit::endBlock()
{
    const int N = curData != nullptr ? curData->numSamples() : 0;
    for (int i = 0; i < kNumVoices; ++i)
    {
        const auto& v = voices[(size_t) i];
        playheads[(size_t) i].store (v.isActive() && N > 0 ? (float) juce::jlimit (0.0, 1.0, v.sourcePosition() / (double) N) : -1.f,
                                     std::memory_order_relaxed);
    }
}
}
