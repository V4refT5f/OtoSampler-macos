#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace oto;

OtoProcessor::OtoProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "OtoSamplerState", createLayout())
{
    auto get = [this] (const char* id) { return apvts.getRawParameterValue (id); };
    pVolume = get (ids::volume); pPitch = get (ids::pitch); pFine = get (ids::fine); pRoot = get (ids::root);
    pNormalize = get (ids::normalize); pStart = get (ids::start); pVelocity = get (ids::velocity);
    pLoopOn = get (ids::loopOn); pLoopStart = get (ids::loopStart); pLoopEnd = get (ids::loopEnd); pLoopXfade = get (ids::loopXfade);
    pFormant = get (ids::formantHold); pSpeed = get (ids::speedHold); pFlatten = get (ids::flatten); pDenoise = get (ids::denoise);
    pAttack = get (ids::attack); pDecay = get (ids::decay); pSustain = get (ids::sustain); pRelease = get (ids::release);
    pVibRate = get (ids::vibRate); pVibDepth = get (ids::vibDepth); pVibDelay = get (ids::vibDelay);
    pGlide = get (ids::glide); pMono = get (ids::mono); pBend = get (ids::bendRange);
    pPreOn = get (ids::preOn); pPreHp = get (ids::preHp); pPreLp = get (ids::preLp);

    for (auto* id : { ids::preOn, ids::preHp, ids::preLp })
        apvts.addParameterListener (id, this);

    for (auto& p : playheads) p.store (-1.f);
    startTimerHz (20);
}

OtoProcessor::~OtoProcessor()
{
    stopTimer();
    for (auto* id : { ids::preOn, ids::preHp, ids::preLp })
        apvts.removeParameterListener (id, this);
    pool.removeAllJobs (true, 10000);
}

void OtoProcessor::parameterChanged (const juce::String&, float) { filterDirty = true; }

bool OtoProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        || l.getMainOutputChannelSet() == juce::AudioChannelSet::mono();
}

void OtoProcessor::prepareToPlay (double sr, int)
{
    hostSr = sr;
    for (auto& v : voices) v.prepare (sr);
    hasLast = false;
}

// ======================= sample loading =======================
std::shared_ptr<SampleSource> OtoProcessor::decode (const juce::MemoryBlock& mb, const juce::String& name)
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
    analyzeLpc (mono, src->sampleRate, src->map, src->lpc);   // spectral envelope + pulse positions (formant-preserving engine)
    return src;
}

std::shared_ptr<SampleData> OtoProcessor::buildSampleData (std::shared_ptr<const SampleSource> src, const FilterSettings& f)
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
    computeResiduals (*d);   // LPC residual of the pre-filtered channels
    return d;
}

FilterSettings OtoProcessor::readFilterSettings() const
{
    FilterSettings f;
    f.on = pPreOn->load() > 0.5f;
    f.hp = pPreHp->load();
    f.lp = pPreLp->load();
    return f;
}

void OtoProcessor::publish (std::shared_ptr<const SampleData> d, bool newSource)
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

void OtoProcessor::loadSampleFromFile (const juce::File& f)
{
    auto mb = std::make_shared<juce::MemoryBlock>();
    if (! f.loadFileAsData (*mb)) return;
    loadSampleFromMemory (mb, f.getFullPathName(), f.getFileName());
}

void OtoProcessor::loadSampleFromMemory (std::shared_ptr<juce::MemoryBlock> mb, juce::String path, juce::String name)
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

void OtoProcessor::rebuildFiltered()
{
    std::shared_ptr<const SampleSource> src;
    { const juce::ScopedLock sl (lock); src = currentSource; }
    if (src != nullptr) publish (buildSampleData (src, readFilterSettings()), false);
}

std::shared_ptr<const SampleData> OtoProcessor::getSampleData() const
{
    const juce::ScopedLock sl (lock);
    return owned;
}

int OtoProcessor::detectRootNote() const
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

juce::String OtoProcessor::getSampleName() const
{
    const juce::ScopedLock sl (lock);
    return sampleName;
}

float OtoProcessor::getStartLimit() const
{
    const auto d = getSampleData();
    if (d == nullptr || d->source == nullptr) return 1.0f;
    return maxStartNorm (d->source->map, d->numSamples(),
                         apvts.getRawParameterValue (ids::loopOn)->load() > 0.5f,
                         apvts.getRawParameterValue (ids::loopStart)->load(),
                         apvts.getRawParameterValue (ids::loopEnd)->load());
}

// Start is pulled back whenever it is beyond the loop start point (loop marker moved, loop switched on, ...)
void OtoProcessor::enforceStartLimit()
{
    const float lim = getStartLimit();
    auto* ps = apvts.getParameter (ids::start);
    if (ps != nullptr && ps->getValue() > lim + 1.0e-6f)
    {
        ps->beginChangeGesture();
        ps->setValueNotifyingHost (lim);
        ps->endChangeGesture();
    }
}

void OtoProcessor::timerCallback()
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
void OtoProcessor::readParams()
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

void OtoProcessor::noteOn (int note, float vel, const SampleData* d)
{
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

void OtoProcessor::noteOff (int note)
{
    for (auto& v : voices)
        if (v.isActive() && v.isGated() && ! v.preview && v.note() == note)
        {
            if (pedalDown) v.pedalHeld = true; else v.release();
        }
}

void OtoProcessor::handleMidi (const juce::MidiMessage& m, const SampleData* d)
{
    if (m.isNoteOn())            noteOn (m.getNoteNumber(), m.getFloatVelocity(), d);
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

void OtoProcessor::renderVoices (juce::AudioBuffer<float>& buf, const SampleData* d, int start, int num)
{
    if (d == nullptr || num <= 0) return;
    float* L = buf.getWritePointer (0) + start;
    float* R = buf.getNumChannels() > 1 ? buf.getWritePointer (1) + start : nullptr;
    const float bend = bendNorm * bendRangeSt;
    for (auto& v : voices)
        if (v.isActive()) v.render (*d, vp, bend, L, R, num);
}

void OtoProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    const int numSamples = buffer.getNumSamples();

    readParams();
    const SampleData* d = audioData.load (std::memory_order_acquire);

    const int sv = sourceVersion.load();
    if (sv != seenSourceVersion) { for (auto& v : voices) v.kill(); seenSourceVersion = sv; hasLast = false; }

    const unsigned pOn = previewOn.load(), pOff = previewOff.load();
    if (pOn != seenPreviewOn)
    {
        seenPreviewOn = pOn;
        if (d != nullptr)
        {
            noteOn ((int) pRoot->load(), 0.8f, d);
            if (lastVoice >= 0) voices[(size_t) lastVoice].preview = true;
        }
    }
    if (pOff != seenPreviewOff)
    {
        seenPreviewOff = pOff;
        for (auto& v : voices) if (v.preview && v.isActive() && v.isGated()) v.release();
    }

    int pos = 0;
    for (const auto meta : midi)
    {
        const int t = juce::jlimit (0, numSamples, meta.samplePosition);
        if (t > pos) { renderVoices (buffer, d, pos, t - pos); pos = t; }
        handleMidi (meta.getMessage(), d);
    }
    renderVoices (buffer, d, pos, numSamples - pos);

    for (int i = 0; i < kNumVoices; ++i)
    {
        const auto& v = voices[(size_t) i];
        const int N = d != nullptr ? d->numSamples() : 0;
        playheads[(size_t) i].store (v.isActive() && N > 0 ? (float) juce::jlimit (0.0, 1.0, v.sourcePosition() / (double) N) : -1.f,
                                     std::memory_order_relaxed);
    }
}

// ======================= state =======================
void OtoProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    {
        const juce::ScopedLock sl (lock);
        state.setProperty ("samplePath", samplePath, nullptr);
        state.setProperty ("sampleName", sampleName, nullptr);
        if (rawFile != nullptr && rawFile->getSize() <= 12u * 1024u * 1024u)   // embed the file so projects are portable
            state.setProperty ("sampleData", rawFile->toBase64Encoding(), nullptr);
    }
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, dest);
}

void OtoProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return;
    auto vt = juce::ValueTree::fromXml (*xml);

    const auto b64 = vt.getProperty ("sampleData").toString();
    const auto path = vt.getProperty ("samplePath").toString();
    const auto name = vt.getProperty ("sampleName").toString();
    vt.removeProperty ("sampleData", nullptr);
    apvts.replaceState (vt);

    juce::MemoryBlock mb;
    if (b64.isNotEmpty() && mb.fromBase64Encoding (b64))
        loadSampleFromMemory (std::make_shared<juce::MemoryBlock> (std::move (mb)), path, name);
    else if (path.isNotEmpty() && juce::File (path).existsAsFile())
        loadSampleFromFile (juce::File (path));
    filterDirty = true;
}

juce::AudioProcessorEditor* OtoProcessor::createEditor() { return new OtoEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new OtoProcessor(); }
