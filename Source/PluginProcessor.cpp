#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr double fadeMs = 3.0;        // retrigger crossfade / start fade-in
    constexpr int peakBins = 1024;        // waveform resolution for the UI
    constexpr double maxStemSeconds = 120.0;
}

DrosakaVoiceProcessor::DrosakaVoiceProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    formatManager.registerBasicFormats();   // wav, aiff, flac, ogg, (mp3/m4a via CoreAudio on mac)
}

bool DrosakaVoiceProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainInputChannelSet().isDisabled();
}

void DrosakaVoiceProcessor::prepareToPlay (double sampleRate, int)
{
    hostRate = sampleRate;
    fadeSamples = juce::jmax (1, (int) std::round (fadeMs * 0.001 * sampleRate));
    for (auto& v : voices) v = {};
    playhead = -1.0f;
}

//==============================================================================
bool DrosakaVoiceProcessor::isSupportedAudioFile (const juce::String& path)
{
    return path.endsWithIgnoreCase (".wav")  || path.endsWithIgnoreCase (".aif")
        || path.endsWithIgnoreCase (".aiff") || path.endsWithIgnoreCase (".flac")
        || path.endsWithIgnoreCase (".ogg")  || path.endsWithIgnoreCase (".mp3")
        || path.endsWithIgnoreCase (".m4a");
}

bool DrosakaVoiceProcessor::loadStem (const juce::File& file, juce::String& errorOut)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr)
    {
        errorOut = "Can't read " + file.getFileName();
        return false;
    }
    if (reader->lengthInSamples <= 0 || reader->sampleRate <= 0)
    {
        errorOut = file.getFileName() + " is empty";
        return false;
    }
    if (reader->lengthInSamples > (juce::int64) (maxStemSeconds * reader->sampleRate))
    {
        errorOut = file.getFileName() + " is longer than 2 minutes";
        return false;
    }

    Stem::Ptr s = new Stem();
    const int numCh = (int) juce::jmin (2u, reader->numChannels);
    const int len = (int) reader->lengthInSamples;
    s->audio.setSize (numCh, len);
    reader->read (&s->audio, 0, len, 0, true, numCh > 1);
    s->sampleRate = reader->sampleRate;
    s->file = file;

    // Waveform peaks (min/max per bin, channels summed).
    s->peaks.assign (peakBins * 2, 0.0f);
    for (int b = 0; b < peakBins; ++b)
    {
        const int a = (int) ((juce::int64) b * len / peakBins);
        const int e = juce::jmax (a + 1, (int) ((juce::int64) (b + 1) * len / peakBins));
        float lo = 0.0f, hi = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto r = s->audio.findMinMax (ch, a, juce::jmin (e, len) - a);
            lo = juce::jmin (lo, r.getStart());
            hi = juce::jmax (hi, r.getEnd());
        }
        s->peaks[(size_t) b * 2] = lo;
        s->peaks[(size_t) b * 2 + 1] = hi;
    }

    Stem::Ptr old;
    {
        const juce::SpinLock::ScopedLockType sl (stemLock);
        old = stem;
        stem = s;
        for (auto& v : voices) v.active = false;   // never play old positions into new audio
    }
    playhead = -1.0f;
    { const juce::ScopedLock l (pathLock); missingPath.clear(); }
    ++stemGeneration;
    // 'old' is released here, on the message thread, never on the audio thread.
    return true;
}

Stem::Ptr DrosakaVoiceProcessor::getStem() const
{
    const juce::SpinLock::ScopedLockType sl (stemLock);
    return stem;
}

//==============================================================================
void DrosakaVoiceProcessor::startVoice()
{
    // Fade out whatever is playing, start a fresh voice from the top.
    auto& prev = voices[current];
    if (prev.active)
    {
        prev.gainTarget = 0.0f;
        prev.gainStep = -prev.gain / (float) fadeSamples;
    }

    current ^= 1;
    auto& v = voices[current];
    v.active = true;
    v.pos = 0.0;
    v.gain = 0.0f;
    v.gainTarget = 1.0f;
    v.gainStep = 1.0f / (float) fadeSamples;
}

void DrosakaVoiceProcessor::renderVoices (juce::AudioBuffer<float>& out, int start, int num, const Stem& s)
{
    const int len = s.audio.getNumSamples();
    const int srcCh = s.audio.getNumChannels();
    const double step = s.sampleRate / hostRate;   // plays at the file's natural speed
    const double endFadeLen = fadeSamples * step;  // in source samples
    auto* outL = out.getWritePointer (0);
    auto* outR = out.getWritePointer (1);
    const float* inL = s.audio.getReadPointer (0);
    const float* inR = s.audio.getReadPointer (srcCh > 1 ? 1 : 0);

    for (auto& v : voices)
    {
        if (! v.active) continue;

        for (int i = start; i < start + num; ++i)
        {
            const int i0 = (int) v.pos;
            if (i0 >= len - 1) { v.active = false; break; }

            const float frac = (float) (v.pos - i0);
            const float l = inL[i0] + frac * (inL[i0 + 1] - inL[i0]);
            const float r = inR[i0] + frac * (inR[i0 + 1] - inR[i0]);

            // Short fade at the stem's end so tightly trimmed files don't click.
            const float tail = (float) juce::jmin (1.0, (len - 1 - v.pos) / endFadeLen);
            const float g = v.gain * tail;
            outL[i] += l * g;
            outR[i] += r * g;

            if (! juce::exactlyEqual (v.gainStep, 0.0f))
            {
                v.gain += v.gainStep;
                if ((v.gainStep > 0.0f && v.gain >= v.gainTarget) || (v.gainStep < 0.0f && v.gain <= v.gainTarget))
                {
                    v.gain = v.gainTarget;
                    v.gainStep = 0.0f;
                    if (v.gain <= 0.0f) { v.active = false; break; }
                }
            }
            v.pos += step;
        }
    }
}

void DrosakaVoiceProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    const int numSamples = buffer.getNumSamples();

    const juce::SpinLock::ScopedTryLockType sl (stemLock);
    if (! sl.isLocked() || stem == nullptr || buffer.getNumChannels() < 2)
    {
        playhead = -1.0f;
        return;
    }
    const Stem& s = *stem;

    // Sample-accurate: render up to each note-on, then (re)trigger.
    int cursor = 0;
    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        if (! msg.isNoteOn()) continue;   // note pitch and note-off are ignored in M1

        const int pos = juce::jlimit (0, numSamples, meta.samplePosition);
        if (pos > cursor) renderVoices (buffer, cursor, pos - cursor, s);
        cursor = pos;
        startVoice();
    }
    if (cursor < numSamples) renderVoices (buffer, cursor, numSamples - cursor, s);

    const auto& v = voices[current];
    playhead = v.active ? (float) (v.pos / s.audio.getNumSamples()) : -1.0f;
}

//==============================================================================
// State: for now just the stem's file path, so the Live set reopens with it.
// (Embedded audio in presets is Milestone 7.)
void DrosakaVoiceProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::ValueTree state ("DrosakaVoice");
    state.setProperty ("version", 1, nullptr);
    if (auto s = getStem())
        state.setProperty ("stemPath", s->file.getFullPathName(), nullptr);
    else if (auto missing = getMissingPath(); missing.isNotEmpty())
        state.setProperty ("stemPath", missing, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void DrosakaVoiceProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr) return;

    auto state = juce::ValueTree::fromXml (*xml);
    const juce::String path = state.getProperty ("stemPath").toString();
    if (path.isNotEmpty())
    {
        juce::File f (path);
        juce::String err;
        if (! f.existsAsFile() || ! loadStem (f, err))
        {
            const juce::ScopedLock l (pathLock);
            missingPath = path;
        }
        ++stemGeneration;   // editor refreshes either way
    }
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
