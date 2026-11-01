#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <limits>
#include <cstring>

namespace
{
    constexpr double fadeMs = 3.0;          // retrigger crossfade, start/end fades
    constexpr double gainGlideSec = 0.05;   // crowd gain: full 0->1 move takes 50 ms
    constexpr double encodeGlideSec = 0.03; // dry <-> codec crossfade
    constexpr int peakBins = 1024;
    constexpr double maxStemSeconds = 120.0;

    float approach (float cur, float target, float maxStep)
    {
        return cur + juce::jlimit (-maxStep, maxStep, target - cur);
    }

    // Constant-power pan normalised so centre = unity on each side.
    void panGains (float pan, float& l, float& r)
    {
        const float theta = (pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        l = std::cos (theta) * juce::MathConstants<float>::sqrt2;
        r = std::sin (theta) * juce::MathConstants<float>::sqrt2;
    }
}

DrosakaVoiceProcessor::DrosakaVoiceProcessor()
    : AudioProcessor (BusesProperties()
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput ("Input", juce::AudioChannelSet::stereo(), false)          // unused: this is an instrument
                          .withInput ("Sidechain", juce::AudioChannelSet::stereo(), true)),    // recording input
      params (*this, nullptr, "PARAMS", createLayout())
{
    formatManager.registerBasicFormats();
    for (auto& c : ccToParam) c = -1;
    for (auto& c : pendingCc) c = -1.0f;
    startTimerHz (60);   // applies MIDI-learned CC moves on the message thread
    countParam = params.getRawParameterValue ("count");
    encodeParam = params.getRawParameterValue ("encodeCount");
    for (int i = 0; i < SliceSet::maxMarkers; ++i) sliceParams[i] = params.getRawParameterValue ("slice" + juce::String (i + 1));
    tuneAmountParam = params.getRawParameterValue ("tuneAmount");
    tuneRootParam = params.getRawParameterValue ("tuneRoot");
    tuneScaleParam = params.getRawParameterValue ("tuneScale");
    recordParam = params.getRawParameterValue ("record");
    takesFolder = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("drosaka Voice").getChildFile ("Takes");
    spreadParam = params.getRawParameterValue ("spread");
    chordParam = params.getRawParameterValue ("chord");
    snapParam = params.getRawParameterValue ("snap");
    spatialParam = params.getRawParameterValue ("spatialMode");
    playModeParam = params.getRawParameterValue ("playMode");
    patternTypeParam = params.getRawParameterValue ("patternType");
    patternDivParam = params.getRawParameterValue ("patternDiv");
    gateParam = params.getRawParameterValue ("gate");
    densityParam = params.getRawParameterValue ("density");
    repeatsParam = params.getRawParameterValue ("repeats");
    dropSemisParam = params.getRawParameterValue ("dropSemis");
    latchParam = params.getRawParameterValue ("latch");
    dropParam = params.getRawParameterValue ("drop");
    keepHumanParam = params.getRawParameterValue ("keepHuman");
    depthParam = params.getRawParameterValue ("depth");
    motionDivParam = params.getRawParameterValue ("motionDiv");
    encodeQuantParam = params.getRawParameterValue ("encodeQuant");
    bitRateParam = params.getRawParameterValue ("bitRate");
    frameDropParam = params.getRawParameterValue ("frameDrop");
    pitchLockParam = params.getRawParameterValue ("pitchLock");
    bank = new Bank();
}

juce::AudioProcessorValueTreeState::ParameterLayout DrosakaVoiceProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "count", 1 }, "Count",
        juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction (
            [] (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; })));

    // Codec.
    auto pct = juce::AudioParameterFloatAttributes().withStringFromValueFunction (
        [] (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; });
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "encodeCount", 1 }, "Encode count",
        juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f, pct));
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "bitRate", 1 }, "Codec bit rate",
        juce::NormalisableRange<float> (0.0f, 1.0f), 0.35f, pct));
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "frameDrop", 1 }, "Codec frame drop",
        juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f, pct));
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "pitchLock", 1 }, "Codec pitch lock",
        juce::StringArray { "Fixed", "Free", "Tracking" }, 2));

    // Spread.
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "spread", 1 }, "Spread",
        juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
        {
            const char* names[] = { "unison", "chorus", "chord", "cluster" };
            const float p = v * 3.0f;
            const int i = juce::jmin (3, juce::roundToInt (p));
            return std::abs (p - (float) i) < 0.05f ? juce::String (names[i]) : juce::String (juce::roundToInt (v * 100.0f)) + "%";
        })));
    juce::StringArray chordNames;
    for (int i = 0; i < spread::numChordSets; ++i) chordNames.add (spread::chordSets()[i].name);
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "chord", 1 }, "Spread chord", chordNames, 1));
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "snap", 1 }, "Spread snap",
        juce::NormalisableRange<float> (0.0f, 1.0f), 0.3f, pct));

    // Space and time (M6).
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "spatialMode", 1 }, "Spatial mode", spatial::modeNames(), 0));
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "motionDiv", 1 }, "Motion rate", spatial::divisionNames(), 4));
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "encodeQuant", 1 }, "Encode quantise", spatial::quantiseNames(), 0));

    // Pattern mode.
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "playMode", 1 }, "Play mode", juce::StringArray { "Manual", "Pattern" }, 0));
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "patternType", 1 }, "Pattern", pattern::typeNames(), 0));
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "patternDiv", 1 }, "Pattern division", pattern::divisionNames(), 1));
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gate", 1 }, "Gate",
        juce::NormalisableRange<float> (0.05f, 1.0f), 0.9f, pct));
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "density", 1 }, "Density",
        juce::NormalisableRange<float> (0.0f, 1.0f), 0.75f, pct));
    layout.add (std::make_unique<juce::AudioParameterInt> (juce::ParameterID { "repeats", 1 }, "Repeat count", 1, 8, 3));
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "dropSemis", 1 }, "Screwed drop",
        juce::NormalisableRange<float> (0.0f, 12.0f), 5.0f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return juce::String (v, 1) + " st"; })));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "latch", 1 }, "Latch", false));

    // The drop, the human primary, and depth.
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "drop", 1 }, "Drop (primary only, human)", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "keepHuman", 1 }, "Keep primary human", false));
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "depth", 1 }, "Depth",
        juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f, pct));

    // Autotune.
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "tuneAmount", 1 }, "Tune",
        juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
            { return v <= 0.001f ? juce::String ("off") : juce::String (juce::roundToInt (v * 100.0f)) + "%"; })));
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "tuneRoot", 1 }, "Tune root", tune::rootNames(), 0));
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "tuneScale", 1 }, "Tune scale", tune::scaleNames(), 0));

    // Record a take from the sidechain input (automatable: punch in on a cue).
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "record", 1 }, "Record take", false));

    // Slice markers: position as a share of the phrase; 0 = unused. Automatable.
    for (int i = 1; i <= SliceSet::maxMarkers; ++i)
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { "slice" + juce::String (i), 1 }, "Slice marker " + juce::String (i),
            juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f,
            juce::AudioParameterFloatAttributes().withStringFromValueFunction (
                [] (float v, int) { return v <= 0.002f ? juce::String ("off") : juce::String (v * 100.0f, 1) + "%"; })));
    return layout;
}

bool DrosakaVoiceProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo()) return false;
    if (! layouts.getMainInputChannelSet().isDisabled()) return false;
    if (layouts.inputBuses.size() > 1)
    {
        const auto sc = layouts.inputBuses[1];
        if (! (sc.isDisabled() || sc == juce::AudioChannelSet::mono() || sc == juce::AudioChannelSet::stereo())) return false;
    }
    return true;
}

void DrosakaVoiceProcessor::prepareToPlay (double sampleRate, int)
{
    hostRate = sampleRate;
    fadeSamples = juce::jmax (1, (int) std::round (fadeMs * 0.001 * sampleRate));
    for (auto& i : instances) i = {};
    playhead = -1.0f;

    latency = LpcCodec::latencySamples (sampleRate);
    setLatencySamples (latency);
    const int ringLen = juce::nextPowerOfTwo (latency + chunkSize + 1);
    for (int v = 0; v < maxVoices; ++v)
    {
        auto& d = voiceDsp[v];
        d.codec.prepare (sampleRate, (juce::uint32) (v + 1));   // fixed seed per slot
        d.delay.assign ((size_t) ringLen, 0.0f);
        d.delayPos = 0;
        d.encode = 0.0f;
        d.drySemis = d.codecSemis = d.psolaMix = 0.0f;
        d.depthRing.assign ((size_t) juce::nextPowerOfTwo ((int) (0.015 * sampleRate) + 4), 0.0f);
        d.depthPos = 0;
        d.depth = 0.0f;
    }
    for (auto& tb : takeBuf) tb.assign ((size_t) (maxTakeSeconds * sampleRate), 0.0f);
    takePos = 0; takeRecording = false; takeFull = false;
    reverb.setSampleRate (sampleRate);
    juce::Reverb::Parameters rp;
    rp.roomSize = 0.55f; rp.damping = 0.45f; rp.wetLevel = 1.0f; rp.dryLevel = 0.0f; rp.width = 1.0f;
    reverb.setParameters (rp);
    reverb.reset();
}

//==============================================================================
bool DrosakaVoiceProcessor::isSupportedAudioFile (const juce::String& path)
{
    for (auto ext : { ".wav", ".aif", ".aiff", ".flac", ".ogg", ".mp3", ".m4a" })
        if (path.endsWithIgnoreCase (ext)) return true;
    return false;
}

Stem::Ptr DrosakaVoiceProcessor::importStem (const juce::File& file, juce::String& error)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    return readStem (reader.get(), file, error, true);
}

// Embedded audio: 24-bit FLAC (lossless), encoded once per stem.
static juce::MemoryBlock encodeFlac (const juce::AudioBuffer<float>& mono, double rate)
{
    juce::MemoryBlock block;
    {
        juce::FlacAudioFormat flac;
        std::unique_ptr<juce::AudioFormatWriter> w (flac.createWriterFor (new juce::MemoryOutputStream (block, false), rate, 1, 24, {}, 5));
        if (w != nullptr) w->writeFromAudioSampleBuffer (mono, 0, mono.getNumSamples());
    }   // the writer finishes the stream when it's destroyed, so leave this scope first
    return block;
}

Stem::Ptr DrosakaVoiceProcessor::readStem (juce::AudioFormatReader* reader, const juce::File& file, juce::String& error, bool encode)
{
    const auto name = file.getFileName();
    if (reader == nullptr)                         { error = "Can't read " + name; return nullptr; }
    if (reader->lengthInSamples <= 1 || reader->sampleRate <= 0) { error = name + " is empty"; return nullptr; }
    if (reader->lengthInSamples > (juce::int64) (maxStemSeconds * reader->sampleRate))
                                                   { error = name + " is longer than 2 minutes"; return nullptr; }

    const int len = (int) reader->lengthInSamples;
    const int numCh = (int) reader->numChannels;
    juce::AudioBuffer<float> raw (numCh, len);
    reader->read (&raw, 0, len, 0, true, true);

    Stem::Ptr s = new Stem();
    s->audio.setSize (1, len);
    s->audio.clear();
    for (int ch = 0; ch < numCh; ++ch) s->audio.addFrom (0, 0, raw, ch, 0, len, 1.0f / (float) numCh);   // downmix
    s->sampleRate = reader->sampleRate;
    s->file = file;

    s->peaks.assign (peakBins * 2, 0.0f);
    for (int b = 0; b < peakBins; ++b)
    {
        const int a = (int) ((juce::int64) b * len / peakBins);
        const int e = juce::jmin (len, juce::jmax (a + 1, (int) ((juce::int64) (b + 1) * len / peakBins)));
        auto r = s->audio.findMinMax (0, a, e - a);
        s->peaks[(size_t) b * 2] = r.getStart();
        s->peaks[(size_t) b * 2 + 1] = r.getEnd();
    }

    s->analysis = analyseStem (s->audio.getReadPointer (0), len, s->sampleRate);
    s->alignEnv = align::envelope (s->audio.getReadPointer (0), len, s->sampleRate);
    s->marks = findPitchMarks (s->audio.getReadPointer (0), len, s->sampleRate);
    if (encode) s->flac = encodeFlac (s->audio, s->sampleRate);
    return s;
}

void DrosakaVoiceProcessor::swapBank (Bank::Ptr b, bool structural)
{
    if (structural) ++structureGeneration;
    Bank::Ptr old;
    {
        const juce::SpinLock::ScopedLockType sl (bankLock);
        old = bank;
        bank = b;
    }
    ++bankGeneration;
    // 'old' is released here, on the message thread.
}

Bank::Ptr DrosakaVoiceProcessor::getBank() const
{
    const juce::SpinLock::ScopedLockType sl (bankLock);
    return bank;
}

int DrosakaVoiceProcessor::addStems (const juce::StringArray& paths, juce::StringArray& errors)
{
    auto b = getBank()->clone();
    int added = 0;
    for (auto& p : paths)
    {
        if (! isSupportedAudioFile (p)) continue;
        if (b->size() >= maxVoices) { errors.add ("Bank is full (" + juce::String (maxVoices) + " voices)"); break; }
        juce::String err;
        if (auto s = importStem (juce::File (p), err))
        {
            b->stems.push_back (s);
            b->flags.push_back ({});
            ++added;
        }
        else errors.add (err);
    }
    if (added > 0)
    {
        b->resort();   // sets a primary if there wasn't one
        for (int i = b->size() - added; i < b->size(); ++i) autoAlign (*b, i);
        swapBank (b);
        const juce::ScopedLock l (pathLock);
        for (auto& s : b->stems) missingPaths.removeString (s->file.getFullPathName());
    }
    return added;
}

void DrosakaVoiceProcessor::removeStem (int idx)
{
    auto b = getBank()->clone();
    if (idx < 0 || idx >= b->size()) return;
    b->stems.erase (b->stems.begin() + idx);
    b->flags.erase (b->flags.begin() + idx);
    if (b->primary == idx) b->primary = b->size() > 0 ? 0 : -1;
    else if (b->primary > idx) --b->primary;
    b->resort();
    swapBank (b);
}

void DrosakaVoiceProcessor::setPrimary (int idx)
{
    auto b = getBank()->clone();
    if (idx < 0 || idx >= b->size()) return;
    if (b->primary >= 0 && b->primary != idx)
    {
        auto& was = b->flags[(size_t) b->primary];   // old reference keeps its place, as if auto-aligned
        was.autoOffsetSec = was.offsetSec;
        was.align = AlignState::autoAligned;
    }
    b->primary = idx;
    b->flags[(size_t) idx].align = AlignState::reference;
    b->resort();   // also recomputes the encode order (primary last)
    swapBank (b);
}

void DrosakaVoiceProcessor::setMute (int idx, bool on)
{
    auto b = getBank()->clone();
    if (idx < 0 || idx >= b->size()) return;
    b->flags[(size_t) idx].mute = on;
    swapBank (b);
}

void DrosakaVoiceProcessor::setSolo (int idx, bool on)
{
    auto b = getBank()->clone();
    if (idx < 0 || idx >= b->size()) return;
    b->flags[(size_t) idx].solo = on;
    swapBank (b);
}

void DrosakaVoiceProcessor::autoAlign (Bank& b, int idx)
{
    auto& f = b.flags[(size_t) idx];
    if (idx == b.primary) { f.align = AlignState::reference; return; }
    const auto& ref = *b.stems[(size_t) b.primary];
    const auto res = align::findOffset (ref.alignEnv, b.stems[(size_t) idx]->alignEnv, b.flags[(size_t) b.primary].offsetSec);
    if (res.confident)
    {
        f.offsetSec = f.autoOffsetSec = res.offsetSec;
        f.align = AlignState::autoAligned;
    }
    else
    {
        f.align = AlignState::unsure;   // probably different words: leave it where it is for the user
    }
}

void DrosakaVoiceProcessor::alignAll()
{
    auto b = getBank()->clone();
    for (int i = 0; i < b->size(); ++i) autoAlign (*b, i);
    swapBank (b, false);
}

void DrosakaVoiceProcessor::setOffset (int idx, double sec)
{
    auto b = getBank()->clone();
    if (idx < 0 || idx >= b->size()) return;
    auto& f = b->flags[(size_t) idx];
    f.offsetSec = juce::jlimit (-30.0, 30.0, sec);
    if (idx != b->primary && f.align != AlignState::unsure) f.align = AlignState::manual;
    swapBank (b, false);
}

void DrosakaVoiceProcessor::resetOffset (int idx)
{
    auto b = getBank()->clone();
    if (idx < 0 || idx >= b->size()) return;
    auto& f = b->flags[(size_t) idx];
    if (idx == b->primary) f.offsetSec = 0.0;
    else if (f.align == AlignState::unsure) f.offsetSec = b->flags[(size_t) b->primary].offsetSec;
    else { f.offsetSec = f.autoOffsetSec; f.align = AlignState::autoAligned; }
    swapBank (b, false);
}

SliceSet DrosakaVoiceProcessor::getSlices() const
{
    float pos[SliceSet::maxMarkers];
    for (int i = 0; i < SliceSet::maxMarkers; ++i) pos[i] = sliceParams[i]->load();
    return SliceSet::fromPositions (pos, getBank()->lengthSeconds());
}

void DrosakaVoiceProcessor::setSliceParam (int slot, float v)
{
    auto* prm = params.getParameter ("slice" + juce::String (slot + 1));
    prm->beginChangeGesture();
    prm->setValueNotifyingHost (v);
    prm->endChangeGesture();
}

void DrosakaVoiceProcessor::addSlice (double sec)
{
    const double len = getBank()->lengthSeconds();
    if (len <= 0.0 || sec <= 0.005 || sec >= len - 0.005) return;
    const auto sl = getSlices();
    for (int i = 0; i < sl.n; ++i) if (std::abs (sl.t[i] - sec) < 0.01) return;   // too close to an existing marker
    for (int slot = 0; slot < SliceSet::maxMarkers; ++slot)
        if (sliceParams[slot]->load() <= 0.002f) { setSliceParam (slot, (float) (sec / len)); return; }
}

void DrosakaVoiceProcessor::moveSlice (int i, double sec)
{
    const auto sl = getSlices();
    const double len = getBank()->lengthSeconds();
    if (i < 0 || i >= sl.n || len <= 0.0) return;
    const double lo = (i > 0 ? sl.t[i - 1] : 0.0) + 0.012;
    const double hi = (i + 1 < sl.n ? sl.t[i + 1] : len) - 0.012;
    if (hi <= lo) return;
    setSliceParam (sl.slot[i], (float) (juce::jlimit (lo, hi, sec) / len));
}

void DrosakaVoiceProcessor::removeSlice (int i)
{
    const auto sl = getSlices();
    if (i < 0 || i >= sl.n) return;
    setSliceParam (sl.slot[i], 0.0f);
}

void DrosakaVoiceProcessor::setSortAxis (SortAxis a)
{
    auto b = getBank()->clone();
    b->axis = a;
    b->resort();
    swapBank (b);
}

//==============================================================================
void DrosakaVoiceProcessor::startInstance (double startPos, double lengthOut, double speed, float pitchSemis)
{
    auto& prev = instances[current];
    if (prev.active)
    {
        prev.envTarget = 0.0f;
        prev.envStep = -juce::jmax (prev.env, 1e-3f) / (float) fadeSamples;
    }
    current ^= 1;
    auto& in = instances[current];
    in.active = true;
    in.startPos = startPos;
    in.out = 0.0;
    in.speed = speed;
    in.stopOut = lengthOut;
    in.pitchSemis = pitchSemis;
    in.releasing = false;
    in.env = 0.0f;
    in.envTarget = 1.0f;
    in.envStep = 1.0f / (float) fadeSamples;
    for (auto& p : in.psola) p = {};
    for (auto& p : in.psolaRaw) p = {};
}

void DrosakaVoiceProcessor::renderDry (int start, int num, Bank& b, const float* psolaRatio, const bool* wantPsola)
{
    const int n = juce::jmin (b.size(), maxVoices);
    const double lookahead = 0.025 * hostRate;   // longest grain half-length (a 40 Hz period)
    const double phraseLen = b.lengthSeconds() * hostRate;

    for (auto& in : instances)
    {
        if (! in.active) continue;

        // A stretched or pitched hit (pattern mode) can't use direct playback: it goes
        // through PSOLA, which changes pitch and time independently.
        const bool forced = std::abs (in.speed - 1.0) > 1e-6 || std::abs (in.pitchSemis) > 1e-4f;
        const float instRatio = std::exp2 (in.pitchSemis / 12.0f);

        // The instance envelope for this segment, shared by every voice. A hit ends with
        // the same short fade, finishing exactly at its length.
        for (int j = 0; j < num; ++j)
        {
            if (! in.releasing && in.out + j >= in.stopOut - fadeSamples)
            {
                in.releasing = true;
                in.envTarget = 0.0f;
                in.envStep = -juce::jmax (in.env, 1e-3f) / (float) fadeSamples;
            }
            envBuf[j] = in.env;
            if (! juce::exactlyEqual (in.envStep, 0.0f))
            {
                in.env += in.envStep;
                if ((in.envStep > 0.0f && in.env >= in.envTarget) || (in.envStep < 0.0f && in.env <= in.envTarget))
                { in.env = in.envTarget; in.envStep = 0.0f; }
            }
        }

        for (int s = 0; s < n; ++s)
        {
            const auto& st = *b.stems[(size_t) s];
            const float* src = st.audio.getReadPointer (0);
            float* dst = dryBuf.getWritePointer (s, start);
            float* sdst = shiftBuf.getWritePointer (s, start);
            const int len = st.audio.getNumSamples();
            const double step = st.sampleRate / hostRate;
            const double endFadeLen = fadeSamples * step;
            const double offset = b.flags[(size_t) s].offsetSec * hostRate;   // alignment shift, host samples
            const auto& mk = st.marks;

            auto read = [&] (double pos) -> float
            {
                const int i0 = (int) std::floor (pos);
                if (i0 < 0 || i0 >= len - 1) return 0.0f;
                const float frac = (float) (pos - i0);
                return src[i0] + frac * (src[i0 + 1] - src[i0]);
            };

            // PSOLA at output time o (samples since the hit), one grain per synthesis pitch mark.
            const float centre = st.analysis.pitchHz;
            const bool tuneThis = liveTune.amount > 0.001f;
            auto psola = [&] (Psola& ps, double o, float ratio, bool applyTune) -> float
            {
                while (ps.nextSynth <= o + lookahead)
                {
                    const double srcTime = (in.startPos + ps.nextSynth * in.speed - offset) * step;
                    if (srcTime >= len) { ps.nextSynth += lookahead; break; }
                    if (srcTime < 0.0) { ps.nextSynth += 0.005 * hostRate; continue; }

                    auto it = std::lower_bound (mk.pos.begin(), mk.pos.end(), (int) srcTime);
                    size_t idx = (size_t) (it - mk.pos.begin());
                    if (idx >= mk.pos.size()) idx = mk.pos.size() - 1;
                    if (idx > 0 && std::abs (mk.pos[idx - 1] - srcTime) < std::abs (mk.pos[idx] - srcTime)) --idx;

                    const bool voiced = mk.voiced[idx] != 0;
                    float r = voiced ? ratio : 1.0f;
                    if (voiced && applyTune && tuneThis)   // autotune each period toward its target
                        r *= tune::ratio ((float) (st.sampleRate / mk.period[idx]), liveTune.amount, liveTune.pitchClass, liveTune.scale, centre);
                    const double half = mk.period[idx] / step;   // one period, in host samples

                    if (ps.count == 8) { for (int k = 1; k < 8; ++k) ps.grains[k - 1] = ps.grains[k]; ps.count = 7; }
                    ps.grains[ps.count++] = { ps.nextSynth, (double) mk.pos[idx], half, voiced ? 1.0f / std::sqrt (r) : 1.0f };
                    ps.nextSynth += half / r;
                }
                float y = 0.0f;
                int keep = 0;
                for (int k = 0; k < ps.count; ++k)
                {
                    const auto& gr = ps.grains[k];
                    const double d = o - gr.synthCenter;
                    if (d >= gr.half) continue;   // finished: drop it
                    ps.grains[keep++] = gr;
                    if (d <= -gr.half) continue;  // not started yet
                    const float w = 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (d / gr.half));
                    y += w * gr.gain * read (gr.srcCenter + d * step);
                }
                ps.count = keep;
                return y;
            };

            const bool canPsola = ! mk.pos.empty();
            for (int j = 0; j < num; ++j)
            {
                const double o = in.out + j;
                const double phrasePos = in.startPos + o * in.speed;
                const double pos = (phrasePos - offset) * step;
                if (pos >= len - 1) break;
                const float edge = (float) juce::jmax (0.0, juce::jmin (1.0, (len - 1 - pos) / endFadeLen, pos / endFadeLen));
                const float g = envBuf[j] * edge;

                if (! forced || ! canPsola)
                {
                    if (g > 0.0f) dst[j] += g * read (pos);
                    if (wantPsola[s] && canPsola) sdst[j] += g * psola (in.psola[s], o, psolaRatio[s], true);
                }
                else
                {
                    // dryBuf (which also feeds the codec) carries the hit's own pitch and time;
                    // shiftBuf adds Spread on top.
                    dst[j] += g * psola (in.psolaRaw[s], o, instRatio, false);   // the codec's feed: it retunes itself
                    if (wantPsola[s]) sdst[j] += g * psola (in.psola[s], o, psolaRatio[s] * instRatio, true);
                }
            }
        }

        in.out += num;
        const double phraseNow = in.startPos + in.out * in.speed;
        if ((juce::exactlyEqual (in.envStep, 0.0f) && in.env <= 0.0f) || phraseNow >= phraseLen) in.active = false;
    }
}

void DrosakaVoiceProcessor::patternNote (const juce::MidiMessage& msg, double ppqAt, const Bank&)
{
    if (msg.isNoteOn())
    {
        const int note = msg.getNoteNumber();
        held[note] = true;
        const int k = note - Bank::sliceRoot;
        const bool sliceKey = liveSlices.numRegions() > 0 && k >= 0 && k < liveSlices.numRegions();
        if (! patternRunning)
        {
            // Start on the next grid line, from the slice played (or the top).
            const double div = pattern::divisionBeats ((int) patternDivParam->load());
            patternState = {};
            patternState.slice = sliceKey ? k : 0;
            lastPatternType = juce::jlimit (0, 6, (int) patternTypeParam->load());
            nextStepPpq = std::ceil (ppqAt / div - 1e-9) * div;
            patternState.startPpq = nextStepPpq;
            numPending = 0;
            patternRunning = true;
        }
        else if (sliceKey)
        {
            // Holding a slice key: the walk goes there on the next step. (Steps are only
            // planned ~10 ms ahead, so 'next step' really is the next one.)
            patternState.jump = k;
        }
    }
    else if (msg.isNoteOff())
    {
        held[msg.getNoteNumber()] = false;
        bool any = false;
        for (bool h : held) any |= h;
        if (! any && latchParam->load() < 0.5f) { patternRunning = false; numPending = 0; }
    }
    else if (msg.isAllNotesOff() || msg.isAllSoundOff())
    {
        for (auto& h : held) h = false;
        patternRunning = false;
        numPending = 0;
    }
}

void DrosakaVoiceProcessor::fireHit (const pattern::Hit& h, const Bank& b, double bpm)
{
    double a = 0.0, z = b.lengthSeconds();
    if (liveSlices.numRegions() > 0) liveSlices.region (Bank::sliceRoot + h.slice, b.lengthSeconds(), a, z);
    const double sliceSec = juce::jmax (0.01, z - a);
    const double stepSec = h.length * 60.0 / bpm;
    const double gate = gateParam->load();

    // Fit the slice to its step: stretch short ones (up to 2x), truncate long ones;
    // Screwed fits exactly (and is pitched down).
    double speed = 1.0;
    if (h.fit) speed = juce::jlimit (0.25, 4.0, sliceSec / stepSec);
    else if (sliceSec < stepSec) speed = juce::jmax (0.5, sliceSec / stepSec);
    const double playSec = juce::jmax (0.02, juce::jmin (stepSec * gate, sliceSec / speed));

    startInstance (a * hostRate, playSec * hostRate, speed, h.pitch);
    patternSlice = h.slice;
}

void DrosakaVoiceProcessor::captureTake (const juce::AudioBuffer<float>& buffer)
{
    const bool want = recordParam->load() > 0.5f && ! takeBuf[0].empty();
    if (want && ! takeRecording)
    {
        takeRecording = true;
        takeFull = false;
        takePos = 0;
    }
    if (takeRecording && want && ! takeFull)
    {
        // Mono sum of the sidechain (silence if nothing is routed in).
        auto* sc = getBusCount (true) > 1 ? getBus (true, 1) : nullptr;
        const int n = buffer.getNumSamples();
        auto& tb = takeBuf[takeIndex];
        const int room = (int) tb.size() - takePos;
        const int m = juce::jmin (n, room);
        float peak = 0.0f;
        if (sc != nullptr && sc->isEnabled() && sc->getNumberOfChannels() > 0)
        {
            auto in = getBusBuffer (const_cast<juce::AudioBuffer<float>&> (buffer), true, 1);
            const int chans = in.getNumChannels();
            for (int i = 0; i < m; ++i)
            {
                float x = 0.0f;
                for (int ch = 0; ch < chans; ++ch) x += in.getSample (ch, i);
                x /= (float) chans;
                tb[(size_t) (takePos + i)] = x;
                peak = juce::jmax (peak, std::abs (x));
            }
        }
        else std::fill (tb.begin() + takePos, tb.begin() + takePos + m, 0.0f);
        takePos += m;
        recordLevel = peak;
        recordSeconds = (float) (takePos / hostRate);
        if (m < n) { takeFull = true; stopRequested = true; }
    }
    if (takeRecording && (! want || takeFull) && finishedTake.load() < 0)
    {
        // Hand the take to the message thread and switch buffers for the next one.
        finishedLength = takePos;
        finishedTake = takeIndex;
        takeIndex ^= 1;
        takeRecording = false;
        recordLevel = 0.0f;
    }
    recordingNow = takeRecording;
}

void DrosakaVoiceProcessor::processFinishedTake()
{
    if (stopRequested.exchange (false))
    {
        auto* prm = params.getParameter ("record");
        prm->beginChangeGesture(); prm->setValueNotifyingHost (0.0f); prm->endChangeGesture();
    }
    const int idx = finishedTake.load();
    if (idx < 0) return;
    const int len = finishedLength.load();
    const auto& tb = takeBuf[idx];

    // Trim silence: keep from 30 ms before the first sound to 80 ms after the last.
    const int win = juce::jmax (1, (int) (0.01 * hostRate));
    std::vector<float> rms;
    float loudest = 0.0f;
    for (int i = 0; i + win <= len; i += win)
    {
        double e = 0.0;
        for (int j = 0; j < win; ++j) e += (double) tb[(size_t) (i + j)] * tb[(size_t) (i + j)];
        rms.push_back ((float) std::sqrt (e / win));
        loudest = juce::jmax (loudest, rms.back());
    }
    const float gate = juce::jmax (loudest * 0.03f, 3e-4f);   // -30 dB below the take, and above -70 dBFS
    int first = -1, last = -1;
    for (int k = 0; k < (int) rms.size(); ++k) if (rms[(size_t) k] > gate) { if (first < 0) first = k; last = k; }

    juce::String message;
    if (first < 0)
    {
        message = "Nothing recorded: route a track into this device's Sidechain input, then record";
    }
    else
    {
        const int a = juce::jmax (0, first * win - (int) (0.03 * hostRate));
        const int z = juce::jmin (len, (last + 1) * win + (int) (0.08 * hostRate));
        juce::AudioBuffer<float> take (1, z - a);
        take.copyFrom (0, 0, tb.data() + a, z - a);
        take.applyGainRamp (0, 0, juce::jmin (64, z - a), 0.0f, 1.0f);
        take.applyGainRamp (0, juce::jmax (0, (z - a) - 256), juce::jmin (256, z - a), 1.0f, 0.0f);

        auto folder = getTakesFolder();
        folder.createDirectory();
        const auto name = "take " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M%S") + "-" + juce::String (++takeCounter);
        auto file = folder.getChildFile (name + ".wav");
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (file), hostRate, 1, 24, {}, 0));
            if (w != nullptr) w->writeFromAudioSampleBuffer (take, 0, take.getNumSamples());
        }
        juce::StringArray errs;
        if (addStems ({ file.getFullPathName() }, errs) == 1)
            message = "Recorded " + juce::String (take.getNumSamples() / hostRate, 1) + " s: " + file.getFileNameWithoutExtension();
        else
            message = errs.joinIntoString (" ");
    }
    { const juce::ScopedLock l (pathLock); recordMessage = message; }
    finishedTake = -1;
    ++bankGeneration;
}

void DrosakaVoiceProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    captureTake (buffer);   // before the buffer is cleared for output: it holds the sidechain input
    buffer.clear();

    // MIDI-learned controls work whether or not a bank is loaded.
    for (const auto meta : midi)
        if (meta.getMessage().isController()) handleCc (meta.getMessage());
    const int numSamples = buffer.getNumSamples();
    if (numSamples == 0 || buffer.getNumChannels() < 2) return;

    const juce::SpinLock::ScopedTryLockType sl (bankLock);
    if (! sl.isLocked() || bank == nullptr || bank->size() == 0)
    {
        playhead = -1.0f;
        for (auto& l : voiceLevel) l = 0.0f;
        return;
    }
    Bank& b = *bank;
    const int n = juce::jmin (b.size(), maxVoices);
    {
        float pos[SliceSet::maxMarkers];
        for (int i = 0; i < SliceSet::maxMarkers; ++i) pos[i] = sliceParams[i]->load();
        liveSlices = SliceSet::fromPositions (pos, b.lengthSeconds());
    }
    liveTune.amount = tuneAmountParam->load();
    liveTune.pitchClass = juce::jlimit (0, 11, (int) tuneRootParam->load());
    liveTune.scale = juce::jlimit (0, 2, (int) tuneScaleParam->load());
    const bool tuneOn = liveTune.amount > 0.001f;

    // Crowd targets for this block (encode and pans are per chunk: they follow the grid).
    float tg[maxVoices], tpRadiate[maxVoices], tp[maxVoices], te[maxVoices];
    computeCrowd (b, countParam->load() * (float) n, tg, tpRadiate);

    // Drop: everything but the primary fades out, and the primary is human (dry).
    const bool dropOn = dropParam->load() > 0.5f;
    const int prim = (b.primary >= 0 && b.primary < n) ? b.primary : -1;
    if (dropOn)
        for (int s = 0; s < n; ++s) { tg[s] = s == prim ? 1.0f : 0.0f; tpRadiate[s] = 0.0f; }
    const bool keepHuman = keepHumanParam->load() > 0.5f;

    // Encode counts audible voices; with "keep primary human" the primary isn't one of them.
    float encodeGain[maxVoices];
    for (int s = 0; s < n; ++s) encodeGain[s] = (keepHuman && s == prim) ? 0.0f : tg[s];
    int ranks[maxVoices];
    int audible = 0;
    for (int s = 0; s < n; ++s) audible += encodeGain[s] > 0.0f ? 1 : 0;
    audibleVoices = audible;

    // Depth: outer voices (by their place in the crowd, not their moving pan, so motion
    // never bends pitch) sit further back: quieter, a little later, more reverb.
    float tDepth[maxVoices];
    {
        int pair[maxVoices], side[maxVoices];
        spread::layout (b, pair, side);
        const int maxPair = juce::jmax (1, n / 2);
        const float depth = depthParam->load();
        for (int s = 0; s < n; ++s) tDepth[s] = dropOn ? 0.0f : depth * (float) pair[s] / (float) maxPair;
    }

    // Musical clock: Live's transport when playing, otherwise an internal clock at the
    // last tempo, so motion keeps moving while stopped. Output is 'latency' late (Live
    // compensates), so grid events are placed on the musical time of the OUTPUT sample.
    double bpm = lastBpm;
    bool playing = false;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto t = pos->getBpm()) bpm = juce::jlimit (20.0, 400.0, *t);
            playing = pos->getIsPlaying();
            if (playing)
                if (auto q = pos->getPpqPosition()) internalPpq = *q;
        }
    lastBpm = bpm;
    hostBpm = (float) bpm;
    patternActive = patternRunning;
    hostPlaying = playing;
    const double spb = hostRate * 60.0 / bpm;            // samples per beat
    const double blockPpq = internalPpq;                 // host time of this block's first sample (for triggers)
    const double ppq0 = internalPpq - latency / spb;     // musical time of this block's first output sample (for mix changes)
    internalPpq += numSamples / spb;

    // Pattern mode setup for this block.
    const bool patternMode = playModeParam->load() > 0.5f;
    if (! patternMode && patternRunning) { patternRunning = false; numPending = 0; }
    if (patternRunning)
    {
        bool any = false;
        for (bool h : held) any |= h;
        if (! any && latchParam->load() < 0.5f) { patternRunning = false; numPending = 0; }   // Latch turned off
        // Transport jumped (loop, relocate): re-anchor to the grid.
        const double div = pattern::divisionBeats ((int) patternDivParam->load());
        if (nextStepPpq > blockPpq + 8.0 || nextStepPpq < blockPpq - 1.0)
        {
            nextStepPpq = std::ceil (blockPpq / div - 1e-9) * div;
            patternState.startPpq = nextStepPpq;
            numPending = 0;
        }
    }

    const int mode = juce::jlimit (0, 3, (int) spatialParam->load());
    const double motionGrid = spatial::divisionBeats ((int) motionDivParam->load());
    const double encodeGrid = spatial::quantiseBeats ((int) encodeQuantParam->load());
    const float encodeKnob = encodeParam->load();

    // Split points: every grid line inside this block, so changes land exactly on it.
    int splits[96], numSplits = 0;
    double grids[2];
    int numGrids = 0;
    if (mode == spatial::bounce || mode == spatial::swap) grids[numGrids++] = motionGrid;   // stepwise motion
    if (encodeGrid > 0.0) grids[numGrids++] = encodeGrid;
    for (int gi = 0; gi < numGrids; ++gi)
    {
        const double g = grids[gi];
        for (double line = (std::floor (ppq0 / g) + 1.0) * g; ; line += g)
        {
            const int at = (int) std::ceil ((line - ppq0) * spb);
            if (at >= numSamples || numSplits >= 96) break;
            if (at > 0) splits[numSplits++] = at;
        }
    }
    if (patternMode)   // every MIDI event starts a chunk, so pattern notes are handled on their sample
        for (const auto meta : midi)
            if (meta.samplePosition > 0 && meta.samplePosition < numSamples && numSplits < 96) splits[numSplits++] = meta.samplePosition;
    std::sort (splits, splits + numSplits);

    // Spread targets (semitones): full interval for the codec, folded +-7 for dry voices.
    float tCodecSemis[maxVoices], tDrySemis[maxVoices];
    spread::compute (b, spreadParam->load(), (int) chordParam->load(), snapParam->load(), tCodecSemis, tDrySemis);

        const auto lock = (LpcCodec::PitchLock) juce::jlimit (0, 2, (int) pitchLockParam->load());
    for (int s = 0; s < n; ++s)
        voiceDsp[s].codec.setParams (bitRateParam->load(), frameDropParam->load(), lock,
                                     b.stems[(size_t) s]->analysis.pitchHz);

    auto* outL = buffer.getWritePointer (0);
    auto* outR = buffer.getWritePointer (1);
    auto midiIt = midi.cbegin();

    int nextSplit = 0;
    for (int chunkStart = 0, num = 0; chunkStart < numSamples; chunkStart += num)
    {
        while (nextSplit < numSplits && splits[nextSplit] <= chunkStart) ++nextSplit;
        num = juce::jmin (chunkSize, numSamples - chunkStart);
        if (nextSplit < numSplits) num = juce::jmin (num, splits[nextSplit] - chunkStart);

        if (patternMode)
        {
            // Notes at this sample start/stop the walk or queue a jump.
            for (; midiIt != midi.cend() && (*midiIt).samplePosition <= chunkStart; ++midiIt)
                patternNote ((*midiIt).getMessage(), blockPpq + juce::jmax (0, (*midiIt).samplePosition) / spb, b);

            // Plan steps a chunk ahead, fire what's due, and end this chunk at the next hit.
            if (patternRunning)
            {
                pattern::Params pp;
                pp.type = juce::jlimit (0, 6, (int) patternTypeParam->load());
                pp.division = pattern::divisionBeats ((int) patternDivParam->load());
                pp.density = densityParam->load();
                pp.repeats = (int) repeatsParam->load();
                pp.drop = dropSemisParam->load();
                // A new pattern starts its own count (Ratchet accelerates from where you switch to it).
                if (pp.type != lastPatternType)
                {
                    lastPatternType = pp.type;
                    patternState.step = 0;
                    patternState.startPpq = nextStepPpq;
                }
                const double horizon = blockPpq + (chunkStart + chunkSize) / spb;
                while (nextStepPpq < horizon && numPending < 24)
                {
                    pattern::Hit hits[8];
                    int nh = 0;
                    const double len = pattern::nextStep (patternState, pp, liveSlices.numRegions(), nextStepPpq, hits, nh);
                    for (int h = 0; h < nh; ++h) pending[numPending++] = { nextStepPpq + hits[h].offset, hits[h] };
                    nextStepPpq += len;
                }
            }
            int keep = 0;
            for (int h = 0; h < numPending; ++h)
            {
                const double at = (pending[h].ppq - blockPpq) * spb;
                if (at <= chunkStart + 1e-6) fireHit (pending[h].hit, b, bpm);
                else
                {
                    pending[keep++] = pending[h];
                    num = juce::jmin (num, juce::jmax (1, (int) std::ceil (at) - chunkStart));
                }
            }
            numPending = keep;
        }

        // Where we are on the grid at the start of this chunk.
        const double ppqNow = ppq0 + chunkStart / spb;

        // Encode: continuous, or latched to whole voices on each grid line.
        float encodeFraction = encodeKnob;
        if (encodeGrid > 0.0)
        {
            const auto stepIdx = (juce::int64) std::floor (ppqNow / encodeGrid + 1e-7);
            if (stepIdx != lastEncodeStep)
            {
                lastEncodeStep = stepIdx;
                latchedEncodeVoices = std::floor (encodeKnob * (float) audible + 1e-4f);
            }
            encodeFraction = audible > 0 ? juce::jmin (latchedEncodeVoices, (float) audible) / (float) audible : 0.0f;
        }
        else lastEncodeStep = std::numeric_limits<juce::int64>::min();
        computeEncode (b, encodeFraction, encodeGain, te, ranks);
        if (prim >= 0 && (keepHuman || dropOn)) te[prim] = 0.0f;

        // Spatial targets.
        const auto motionStep = (juce::int64) std::floor (ppqNow / motionGrid + 1e-7);
        const double cycle = ppqNow / motionGrid - std::floor (ppqNow / motionGrid);
        spatial::pans (mode, b, tpRadiate, tg, motionStep, cycle, tp);

        // Spread glides (an octave takes 80 ms at most), and whether each voice needs PSOLA.
        const float semiStep = (float) (12.0 * num / (0.08 * hostRate));
        const float mixStep = (float) (num / (0.03 * hostRate));
        float psolaRatio[maxVoices], mix0[maxVoices], mix1[maxVoices];
        bool wantPsola[maxVoices];
        for (int s = 0; s < n; ++s)
        {
            auto& vd = voiceDsp[s];
            vd.drySemis = approach (vd.drySemis, tDrySemis[s], semiStep);
            vd.codecSemis = approach (vd.codecSemis, tCodecSemis[s], semiStep);
            const float mixTarget = tuneOn || std::abs (tDrySemis[s]) > 0.005f || std::abs (vd.drySemis) > 0.005f ? 1.0f : 0.0f;
            mix0[s] = vd.psolaMix;
            vd.psolaMix = approach (vd.psolaMix, mixTarget, mixStep);
            mix1[s] = vd.psolaMix;
            wantPsola[s] = mix0[s] > 0.0f || mix1[s] > 0.0f;
            psolaRatio[s] = std::exp2 (vd.drySemis / 12.0f);
            vd.codec.setPitchRatio (std::exp2 (vd.codecSemis / 12.0f));
            vd.codec.setTune (liveTune, b.stems[(size_t) s]->analysis.pitchHz);
        }

        // Dry voices, with sample-accurate triggers inside this chunk.
        dryBuf.clear (0, num);
        shiftBuf.clear (0, num);
        int cursor = 0;
        for (; ! patternMode && midiIt != midi.cend(); ++midiIt)
        {
            const auto meta = *midiIt;
            const int pos = meta.samplePosition - chunkStart;
            if (pos >= num) break;
            if (! meta.getMessage().isNoteOn()) continue;   // pitch, velocity, note-off ignored
            const int at = juce::jlimit (0, num, pos);
            if (at > cursor) renderDry (cursor, at - cursor, b, psolaRatio, wantPsola);
            cursor = at;
            double a, z;
            liveSlices.region (meta.getMessage().getNoteNumber(), b.lengthSeconds(), a, z);   // a slice key, or the whole phrase
            startInstance (a * hostRate, (z - a) * hostRate, 1.0, 0.0f);
        }
        if (cursor < num) renderDry (cursor, num - cursor, b, psolaRatio, wantPsola);

        // Glide each voice toward its targets over this chunk.
        const float gStep = (float) (num / (gainGlideSec * hostRate));
        const float pStep = (float) (2.0 * num / (spatial::panGlideSeconds (mode) * hostRate));
        const float eStep = (float) (num / (encodeGlideSec * hostRate));
        const float invNum = 1.0f / (float) num;

        std::fill (sendL, sendL + num, 0.0f);
        std::fill (sendR, sendR + num, 0.0f);

        for (int s = 0; s < n; ++s)
        {
            auto& st = *b.stems[(size_t) s];
            auto& vd = voiceDsp[s];

            float l0, r0, l1, r1;
            panGains (st.curPan, l0, r0);
            const float g0 = st.curGain, e0 = vd.encode;
            st.curGain = approach (st.curGain, tg[s], gStep);
            st.curPan = approach (st.curPan, tp[s], pStep);
            vd.encode = approach (vd.encode, te[s], eStep);
            panGains (st.curPan, l1, r1);
            const float g1 = st.curGain, e1 = vd.encode;
            l0 *= g0; r0 *= g0; l1 *= g1; r1 *= g1;

            // Codec runs on every voice continuously, so a voice switching to it is already warm.
            const float* dry = dryBuf.getReadPointer (s);
            vd.codec.process (dry, codecOut.data(), num);

            const int mask = (int) vd.delay.size() - 1;
            const bool silent = l0 + r0 + l1 + r1 <= 0.0f;
            const float* shifted = shiftBuf.getReadPointer (s);

            // Depth glides slowly (it only moves when the crowd's layout does).
            const float d0 = vd.depth;
            vd.depth = approach (vd.depth, tDepth[s], (float) (num / (0.2 * hostRate)));
            const float d1 = vd.depth;
            const int dmask = (int) vd.depthRing.size() - 1;
            const float maxPre = (float) (0.012 * hostRate);   // up to 12 ms later

            for (int j = 0; j < num; ++j)
            {
                // Dry output = direct playback, crossfaded to the pitch-shifted version when Spread moves it.
                const float m = mix0[s] + (mix1[s] - mix0[s]) * (float) j * invNum;
                vd.delay[(size_t) vd.delayPos] = wantPsola[s] ? dry[j] + m * (shifted[j] - dry[j]) : dry[j];
                const float delayed = vd.delay[(size_t) ((vd.delayPos - latency) & mask)];
                vd.delayPos = (vd.delayPos + 1) & mask;

                const float a = (float) j * invNum;
                float x = 0.0f;
                if (! silent)
                {
                    const float e = e0 + a * (e1 - e0);
                    // Equal-power crossfade dry <-> codec.
                    x = delayed * std::cos (e * juce::MathConstants<float>::halfPi)
                      + codecOut[(size_t) j] * std::sin (e * juce::MathConstants<float>::halfPi);
                }

                // Depth: pre-delay (fractional), level and reverb send for outer voices.
                vd.depthRing[(size_t) vd.depthPos] = x;
                const float d = d0 + a * (d1 - d0);
                if (d > 0.0f)
                {
                    const float pre = d * maxPre;
                    const int pi = (int) pre;
                    const float pf = pre - (float) pi;
                    const float x0 = vd.depthRing[(size_t) ((vd.depthPos - pi) & dmask)];
                    const float x1 = vd.depthRing[(size_t) ((vd.depthPos - pi - 1) & dmask)];
                    const float xd = x0 + pf * (x1 - x0);
                    const float send = xd * d * 0.22f;
                    sendL[j] += send * (l0 + a * (l1 - l0));
                    sendR[j] += send * (r0 + a * (r1 - r0));
                    x = xd * (1.0f - 0.5f * d);    // up to -6 dB dry
                }
                vd.depthPos = (vd.depthPos + 1) & dmask;
                if (silent) continue;

                outL[chunkStart + j] += x * (l0 + a * (l1 - l0));
                outR[chunkStart + j] += x * (r0 + a * (r1 - r0));
            }
        }

        // Shared reverb for the depth sends (silent at Depth 0, but kept running so it's warm).
        reverb.processStereo (sendL, sendR, num);
        for (int j = 0; j < num; ++j) { outL[chunkStart + j] += sendL[j]; outR[chunkStart + j] += sendR[j]; }
    }

    for (int s = 0; s < n; ++s) voiceRank[s] = ranks[s];
    for (int s = 0; s < n; ++s)
    {
        voicePan[s] = b.stems[(size_t) s]->curPan;
        voiceLevel[s] = b.stems[(size_t) s]->curGain;
        voiceEncode[s] = voiceDsp[s].encode;
        voiceSemis[s] = voiceDsp[s].drySemis + voiceDsp[s].encode * (voiceDsp[s].codecSemis - voiceDsp[s].drySemis);
    }
    for (int s = n; s < maxVoices; ++s) { voiceLevel[s] = 0.0f; voiceEncode[s] = 0.0f; }

    const auto& in = instances[current];
    const double lenSamples = b.lengthSeconds() * hostRate;
    playhead = in.active && lenSamples > 0.0 ? (float) juce::jmin (1.0, (in.startPos + in.out * in.speed) / lenSamples) : -1.0f;
}

//==============================================================================
// State (v4): a binary ValueTree, magic "DVS4", holding the knobs, the bank with each
// stem's audio embedded as 24-bit FLAC, its flags/offsets, and the slice markers.
// The Live set and preset files both use it, so a phrase bank travels with them.
// Older XML states (v1-v3, file paths only) still load.
namespace { const char stateMagic[4] = { 'D', 'V', 'S', '4' }; }

void DrosakaVoiceProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::ValueTree state ("DrosakaVoice");
    state.setProperty ("version", 4, nullptr);
    state.appendChild (params.copyState(), nullptr);

    auto b = getBank();
    juce::ValueTree bankTree ("Bank");
    bankTree.setProperty ("axis", (int) b->axis, nullptr);
    bankTree.setProperty ("primaryIndex", b->primary, nullptr);
    // (Slice markers are parameters now; they're in the params child above.)
    for (int i = 0; i < b->size(); ++i)
    {
        const auto& st = *b->stems[(size_t) i];
        const auto& f = b->flags[(size_t) i];
        juce::ValueTree v ("Stem");
        v.setProperty ("path", st.file.getFullPathName(), nullptr);
        v.setProperty ("mute", f.mute, nullptr);
        v.setProperty ("solo", f.solo, nullptr);
        v.setProperty ("offset", f.offsetSec, nullptr);
        v.setProperty ("autoOffset", f.autoOffsetSec, nullptr);
        v.setProperty ("align", (int) f.align, nullptr);
        if (st.flac.getSize() > 0) v.setProperty ("flac", juce::var (st.flac), nullptr);
        bankTree.appendChild (v, nullptr);
    }
    for (auto& missing : getMissingPaths())   // old sets: keep paths we couldn't find
    {
        juce::ValueTree v ("Stem");
        v.setProperty ("path", missing, nullptr);
        bankTree.appendChild (v, nullptr);
    }
    state.appendChild (bankTree, nullptr);

    // MIDI-learned controls, by parameter ID ("cc:id,cc:id").
    juce::StringArray map;
    auto& ps = getParameters();
    for (int cc = 0; cc < 128; ++cc)
        if (const int pi = ccToParam[cc].load(); pi >= 0 && pi < ps.size())
            if (auto* p = dynamic_cast<juce::AudioProcessorParameterWithID*> (ps[pi])) map.add (juce::String (cc) + ":" + p->paramID);
    state.setProperty ("midiMap", map.joinIntoString (","), nullptr);

    juce::MemoryOutputStream out (dest, false);
    out.write (stateMagic, 4);
    state.writeToStream (out);
}

void DrosakaVoiceProcessor::setStateInformation (const void* data, int size)
{
    juce::ValueTree state;
    if (size > 4 && std::memcmp (data, stateMagic, 4) == 0)
        state = juce::ValueTree::readFromData (static_cast<const char*> (data) + 4, (size_t) size - 4);
    else if (auto xml = getXmlFromBinary (data, size))
        state = juce::ValueTree::fromXml (*xml);
    if (! state.isValid()) return;

    auto p = state.getChildWithName (params.state.getType());
    if (p.isValid()) params.replaceState (p);

    for (auto& c : ccToParam) c = -1;
    for (auto& entry : juce::StringArray::fromTokens (state.getProperty ("midiMap").toString(), ",", ""))
    {
        const int cc = entry.upToFirstOccurrenceOf (":", false, false).getIntValue();
        const int pi = paramIndex (entry.fromFirstOccurrenceOf (":", false, false));
        if (cc >= 0 && cc < 128 && pi >= 0) ccToParam[cc] = pi;
    }
    ++midiMapGeneration;

    Bank::Ptr b = new Bank();
    juce::StringArray missing;
    juce::FlacAudioFormat flac;
    std::vector<double> legacySlices;

    auto bankTree = state.getChildWithName ("Bank");
    if (bankTree.isValid())
    {
        b->axis = (int) bankTree.getProperty ("axis", 0) == 1 ? SortAxis::formant : SortAxis::pitch;
        const juce::String primaryPath = bankTree.getProperty ("primary").toString();   // v2/v3
        const int primaryIndex = bankTree.getProperty ("primaryIndex", -1);             // v4
        int stemCount = 0;
        for (auto v : bankTree)
        {
            const juce::String path = v.getProperty ("path").toString();
            juce::String err;
            Stem::Ptr s;
            if (auto* blob = v.getProperty ("flac").getBinaryData(); blob != nullptr && blob->getSize() > 0)
            {
                // Embedded audio: no need for the original file.
                std::unique_ptr<juce::AudioFormatReader> r (flac.createReaderFor (new juce::MemoryInputStream (*blob, false), true));
                s = readStem (r.get(), juce::File (path), err, false);
                if (s != nullptr) s->flac = *blob;
            }
            else if (juce::File (path).existsAsFile())
            {
                s = importStem (juce::File (path), err);
            }
            if (s == nullptr) { missing.add (path); ++stemCount; continue; }

            if ((primaryIndex >= 0 && stemCount == primaryIndex) || (primaryIndex < 0 && path == primaryPath))
                b->primary = (int) b->stems.size();
            ++stemCount;
            b->stems.push_back (s);
            VoiceFlags fl;
            fl.mute = (bool) v.getProperty ("mute", false);
            fl.solo = (bool) v.getProperty ("solo", false);
            fl.offsetSec = (double) v.getProperty ("offset", 0.0);
            fl.autoOffsetSec = (double) v.getProperty ("autoOffset", 0.0);
            fl.align = (AlignState) juce::jlimit (0, 3, (int) v.getProperty ("align", 0));
            b->flags.push_back (fl);
        }
        // Sets from before markers were parameters: seconds in the bank tree.
        for (auto& t : juce::StringArray::fromTokens (bankTree.getProperty ("slices").toString(), ",", ""))
            if (t.isNotEmpty()) legacySlices.push_back (t.getDoubleValue());
    }
    else if (state.hasProperty ("stemPath"))   // Milestone 1 sets
    {
        const juce::String path = state.getProperty ("stemPath").toString();
        juce::String err;
        if (auto s = juce::File (path).existsAsFile() ? importStem (juce::File (path), err) : nullptr)
        { b->stems.push_back (s); b->flags.push_back ({}); }
        else missing.add (path);
    }

    b->resort();
    if (b->primary >= 0 && b->primary < b->size()) b->flags[(size_t) b->primary].align = AlignState::reference;
    { const juce::ScopedLock l (pathLock); missingPaths = missing; }
    swapBank (b);

    // Migrate old slice markers into the marker parameters.
    if (! legacySlices.empty())
    {
        const double len = b->lengthSeconds();
        std::sort (legacySlices.begin(), legacySlices.end());
        for (int i = 0; i < SliceSet::maxMarkers; ++i)
            params.getParameter ("slice" + juce::String (i + 1))->setValueNotifyingHost (
                i < (int) legacySlices.size() && len > 0.0 ? (float) (legacySlices[(size_t) i] / len) : 0.0f);
    }
}

//==============================================================================
int DrosakaVoiceProcessor::paramIndex (const juce::String& id) const
{
    auto& ps = getParameters();
    for (int i = 0; i < ps.size(); ++i)
        if (auto* p = dynamic_cast<juce::AudioProcessorParameterWithID*> (ps[i]); p != nullptr && p->paramID == id) return i;
    return -1;
}

void DrosakaVoiceProcessor::armMidiLearn (const juce::String& id) { learnParam = paramIndex (id); ++midiMapGeneration; }

juce::String DrosakaVoiceProcessor::getLearningParam() const
{
    const int i = learnParam.load();
    if (i < 0 || i >= getParameters().size()) return {};
    if (auto* p = dynamic_cast<juce::AudioProcessorParameterWithID*> (getParameters()[i])) return p->paramID;
    return {};
}

int DrosakaVoiceProcessor::getCcFor (const juce::String& id) const
{
    const int pi = paramIndex (id);
    if (pi < 0) return -1;
    for (int cc = 0; cc < 128; ++cc) if (ccToParam[cc].load() == pi) return cc;
    return -1;
}

void DrosakaVoiceProcessor::forgetCc (const juce::String& id)
{
    const int pi = paramIndex (id);
    for (auto& c : ccToParam) if (c.load() == pi) c = -1;
    ++midiMapGeneration;
}

// Audio thread: bind or route a CC. The parameter itself is set on the message thread
// (applyPendingMidi) so the host is notified properly and records automation.
void DrosakaVoiceProcessor::handleCc (const juce::MidiMessage& m)
{
    const int cc = m.getControllerNumber();
    if (cc < 0 || cc > 127) return;
    if (const int learn = learnParam.load(); learn >= 0)
    {
        for (auto& c : ccToParam) if (c.load() == learn) c = -1;   // one CC per parameter
        ccToParam[cc] = learn;
        learnParam = -1;
        ++midiMapGeneration;
    }
    if (ccToParam[cc].load() >= 0) pendingCc[cc] = (float) m.getControllerValue() / 127.0f;
}

void DrosakaVoiceProcessor::applyPendingMidi()
{
    auto& ps = getParameters();
    for (int cc = 0; cc < 128; ++cc)
    {
        const float v = pendingCc[cc].exchange (-1.0f);
        const int pi = ccToParam[cc].load();
        if (v < 0.0f || pi < 0 || pi >= ps.size()) continue;
        auto* p = ps[pi];
        if (std::abs (p->getValue() - v) < 1e-6f) continue;
        p->beginChangeGesture();
        p->setValueNotifyingHost (v);
        p->endChangeGesture();
    }
}

bool DrosakaVoiceProcessor::savePreset (const juce::File& f)
{
    juce::MemoryBlock mb;
    getStateInformation (mb);
    return f.replaceWithData (mb.getData(), mb.getSize());
}

bool DrosakaVoiceProcessor::loadPreset (const juce::File& f)
{
    juce::MemoryBlock mb;
    if (! f.loadFileAsData (mb) || mb.getSize() < 8) return false;
    setStateInformation (mb.getData(), (int) mb.getSize());
    return true;
}

//==============================================================================
juce::AudioProcessorEditor* DrosakaVoiceProcessor::createEditor()
{
    return new DrosakaVoiceEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DrosakaVoiceProcessor();
}
