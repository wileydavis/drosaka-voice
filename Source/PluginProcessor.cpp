#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr double fadeMs = 3.0;          // retrigger crossfade, start/end fades
    constexpr double gainGlideSec = 0.05;   // crowd gain: full 0->1 move takes 50 ms
    constexpr double panGlideSec = 0.10;    // pan: full L->R move takes 100 ms
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
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      params (*this, nullptr, "PARAMS", createLayout())
{
    formatManager.registerBasicFormats();
    countParam = params.getRawParameterValue ("count");
    encodeParam = params.getRawParameterValue ("encodeCount");
    spreadParam = params.getRawParameterValue ("spread");
    chordParam = params.getRawParameterValue ("chord");
    snapParam = params.getRawParameterValue ("snap");
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
    return layout;
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
    }
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
    if (reader == nullptr)                         { error = "Can't read " + file.getFileName(); return nullptr; }
    if (reader->lengthInSamples <= 1 || reader->sampleRate <= 0) { error = file.getFileName() + " is empty"; return nullptr; }
    if (reader->lengthInSamples > (juce::int64) (maxStemSeconds * reader->sampleRate))
                                                   { error = file.getFileName() + " is longer than 2 minutes"; return nullptr; }

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

void DrosakaVoiceProcessor::setSortAxis (SortAxis a)
{
    auto b = getBank()->clone();
    b->axis = a;
    b->resort();
    swapBank (b);
}

//==============================================================================
void DrosakaVoiceProcessor::startInstance()
{
    auto& prev = instances[current];
    if (prev.active)
    {
        prev.envTarget = 0.0f;
        prev.envStep = -prev.env / (float) fadeSamples;
    }
    current ^= 1;
    auto& in = instances[current];
    in.active = true;
    in.t = 0.0;
    in.env = 0.0f;
    in.envTarget = 1.0f;
    in.envStep = 1.0f / (float) fadeSamples;
    for (auto& p : in.psola) p = {};
}

void DrosakaVoiceProcessor::renderDry (int start, int num, Bank& b, const float* psolaRatio, const bool* wantPsola)
{
    const int n = juce::jmin (b.size(), maxVoices);
    const double lookahead = 0.025 * hostRate;   // longest grain half-length (a 40 Hz period)

    // Longest stem, in host samples: an instance ends when every stem has.
    double longest = 0.0;
    for (int s = 0; s < n; ++s)
    {
        const auto& st = *b.stems[(size_t) s];
        longest = juce::jmax (longest, (st.audio.getNumSamples() - 1) * hostRate / st.sampleRate
                                           + juce::jmax (0.0, b.flags[(size_t) s].offsetSec * hostRate));
    }

    for (auto& in : instances)
    {
        if (! in.active) continue;

        // The instance envelope for this segment, shared by every voice.
        for (int j = 0; j < num; ++j)
        {
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

            auto read = [&] (double pos) -> float
            {
                const int i0 = (int) std::floor (pos);
                if (i0 < 0 || i0 >= len - 1) return 0.0f;
                const float frac = (float) (pos - i0);
                return src[i0] + frac * (src[i0 + 1] - src[i0]);
            };

            auto& ps = in.psola[s];
            const auto& mk = st.marks;
            const float ratio = psolaRatio[s];

            for (int j = 0; j < num; ++j)
            {
                const double T = in.t + j;   // host samples since trigger
                const double pos = (T - offset) * step;
                if (pos >= len - 1) break;
                const float edge = (float) juce::jmax (0.0, juce::jmin (1.0, (len - 1 - pos) / endFadeLen, pos / endFadeLen));
                const float g = envBuf[j] * edge;
                if (g <= 0.0f && ! wantPsola[s]) continue;

                dst[j] += g * read (pos);

                if (! wantPsola[s] || mk.pos.empty()) continue;

                // PSOLA: spawn grains ahead of time, one per synthesis pitch mark.
                while (ps.nextSynth <= T + lookahead)
                {
                    const double srcTime = (ps.nextSynth - offset) * step;
                    if (srcTime >= len) { ps.nextSynth += lookahead; break; }
                    if (srcTime < 0.0) { ps.nextSynth += 0.005 * hostRate; continue; }

                    // Nearest analysis mark.
                    auto it = std::lower_bound (mk.pos.begin(), mk.pos.end(), (int) srcTime);
                    size_t idx = (size_t) (it - mk.pos.begin());
                    if (idx >= mk.pos.size()) idx = mk.pos.size() - 1;
                    if (idx > 0 && std::abs (mk.pos[idx - 1] - srcTime) < std::abs (mk.pos[idx] - srcTime)) --idx;

                    const bool voiced = mk.voiced[idx] != 0;
                    const float r = voiced ? ratio : 1.0f;
                    const double half = mk.period[idx] / step;   // one period, in host samples

                    if (ps.count == 8) { for (int k = 1; k < 8; ++k) ps.grains[k - 1] = ps.grains[k]; ps.count = 7; }
                    ps.grains[ps.count++] = { ps.nextSynth, (double) mk.pos[idx], half, voiced ? 1.0f / std::sqrt (r) : 1.0f };
                    ps.nextSynth += half / r;
                }

                // Overlap-add the live grains (Hann, two periods long).
                float y = 0.0f;
                int keep = 0;
                for (int k = 0; k < ps.count; ++k)
                {
                    const auto& gr = ps.grains[k];
                    const double d = T - gr.synthCenter;
                    if (d >= gr.half) continue;   // finished: drop it
                    ps.grains[keep++] = gr;
                    if (d <= -gr.half) continue;  // not started yet
                    const float w = 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (d / gr.half));
                    y += w * gr.gain * read (gr.srcCenter + d * step);
                }
                ps.count = keep;
                sdst[j] += g * y;
            }
        }

        in.t += num;
        if ((juce::exactlyEqual (in.envStep, 0.0f) && in.env <= 0.0f) || in.t >= longest) in.active = false;
    }
}

void DrosakaVoiceProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
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

    // Crowd and codec targets for this block.
    float tg[maxVoices], tp[maxVoices], te[maxVoices];
    computeCrowd (b, countParam->load() * (float) n, tg, tp);
    int ranks[maxVoices];
    computeEncode (b, encodeParam->load(), tg, te, ranks);
    int audible = 0;
    for (int s = 0; s < n; ++s) { voiceRank[s] = ranks[s]; audible += ranks[s] > 0 ? 1 : 0; }
    audibleVoices = audible;

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

    for (int chunkStart = 0; chunkStart < numSamples; chunkStart += chunkSize)
    {
        const int num = juce::jmin (chunkSize, numSamples - chunkStart);

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
            const float mixTarget = std::abs (tDrySemis[s]) > 0.005f || std::abs (vd.drySemis) > 0.005f ? 1.0f : 0.0f;
            mix0[s] = vd.psolaMix;
            vd.psolaMix = approach (vd.psolaMix, mixTarget, mixStep);
            mix1[s] = vd.psolaMix;
            wantPsola[s] = mix0[s] > 0.0f || mix1[s] > 0.0f;
            psolaRatio[s] = std::exp2 (vd.drySemis / 12.0f);
            vd.codec.setPitchRatio (std::exp2 (vd.codecSemis / 12.0f));
        }

        // Dry voices, with sample-accurate triggers inside this chunk.
        dryBuf.clear (0, num);
        shiftBuf.clear (0, num);
        int cursor = 0;
        for (; midiIt != midi.cend(); ++midiIt)
        {
            const auto meta = *midiIt;
            const int pos = meta.samplePosition - chunkStart;
            if (pos >= num) break;
            if (! meta.getMessage().isNoteOn()) continue;   // pitch, velocity, note-off ignored
            const int at = juce::jlimit (0, num, pos);
            if (at > cursor) renderDry (cursor, at - cursor, b, psolaRatio, wantPsola);
            cursor = at;
            startInstance();
        }
        if (cursor < num) renderDry (cursor, num - cursor, b, psolaRatio, wantPsola);

        // Glide each voice toward its targets over this chunk.
        const float gStep = (float) (num / (gainGlideSec * hostRate));
        const float pStep = (float) (2.0 * num / (panGlideSec * hostRate));
        const float eStep = (float) (num / (encodeGlideSec * hostRate));
        const float invNum = 1.0f / (float) num;

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
            for (int j = 0; j < num; ++j)
            {
                // Dry output = direct playback, crossfaded to the pitch-shifted version when Spread moves it.
                const float m = mix0[s] + (mix1[s] - mix0[s]) * (float) j * invNum;
                vd.delay[(size_t) vd.delayPos] = wantPsola[s] ? dry[j] + m * (shifted[j] - dry[j]) : dry[j];
                const float delayed = vd.delay[(size_t) ((vd.delayPos - latency) & mask)];
                vd.delayPos = (vd.delayPos + 1) & mask;
                if (silent) continue;

                const float a = (float) j * invNum;
                const float e = e0 + a * (e1 - e0);
                // Equal-power crossfade dry <-> codec.
                const float x = delayed * std::cos (e * juce::MathConstants<float>::halfPi)
                              + codecOut[(size_t) j] * std::sin (e * juce::MathConstants<float>::halfPi);
                outL[chunkStart + j] += x * (l0 + a * (l1 - l0));
                outR[chunkStart + j] += x * (r0 + a * (r1 - r0));
            }
        }
    }

    for (int s = 0; s < n; ++s)
    {
        voiceLevel[s] = b.stems[(size_t) s]->curGain;
        voiceEncode[s] = voiceDsp[s].encode;
        voiceSemis[s] = voiceDsp[s].drySemis + voiceDsp[s].encode * (voiceDsp[s].codecSemis - voiceDsp[s].drySemis);
    }
    for (int s = n; s < maxVoices; ++s) { voiceLevel[s] = 0.0f; voiceEncode[s] = 0.0f; }

    const auto& in = instances[current];
    const int prim = juce::jlimit (0, n - 1, b.primary);
    const auto& ps = *b.stems[(size_t) prim];
    playhead = in.active ? (float) juce::jmin (1.0, in.t * ps.sampleRate / hostRate / ps.audio.getNumSamples()) : -1.0f;
}

//==============================================================================
// State: knob values + the bank as file paths with flags and cached analysis.
// (Embedded audio in presets is Milestone 7.)
void DrosakaVoiceProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::ValueTree state ("DrosakaVoice");
    state.setProperty ("version", 3, nullptr);
    state.appendChild (params.copyState(), nullptr);

    auto b = getBank();
    juce::ValueTree bankTree ("Bank");
    bankTree.setProperty ("axis", (int) b->axis, nullptr);
    if (b->primary >= 0 && b->primary < b->size())
        bankTree.setProperty ("primary", b->stems[(size_t) b->primary]->file.getFullPathName(), nullptr);
    for (int i = 0; i < b->size(); ++i)
    {
        const auto& s = *b->stems[(size_t) i];
        juce::ValueTree v ("Stem");
        v.setProperty ("path", s.file.getFullPathName(), nullptr);
        v.setProperty ("mute", b->flags[(size_t) i].mute, nullptr);
        v.setProperty ("solo", b->flags[(size_t) i].solo, nullptr);
        v.setProperty ("offset", b->flags[(size_t) i].offsetSec, nullptr);
        v.setProperty ("autoOffset", b->flags[(size_t) i].autoOffsetSec, nullptr);
        v.setProperty ("align", (int) b->flags[(size_t) i].align, nullptr);
        bankTree.appendChild (v, nullptr);
    }
    for (auto& missing : getMissingPaths())   // keep them so saving doesn't lose them
    {
        juce::ValueTree v ("Stem");
        v.setProperty ("path", missing, nullptr);
        bankTree.appendChild (v, nullptr);
    }
    state.appendChild (bankTree, nullptr);

    if (auto xml = state.createXml()) copyXmlToBinary (*xml, dest);
}

void DrosakaVoiceProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr) return;
    auto state = juce::ValueTree::fromXml (*xml);

    auto p = state.getChildWithName (params.state.getType());
    if (p.isValid()) params.replaceState (p);

    Bank::Ptr b = new Bank();
    juce::StringArray missing;

    auto bankTree = state.getChildWithName ("Bank");
    if (bankTree.isValid())
    {
        b->axis = (int) bankTree.getProperty ("axis", 0) == 1 ? SortAxis::formant : SortAxis::pitch;
        const juce::String primaryPath = bankTree.getProperty ("primary").toString();
        for (auto v : bankTree)
        {
            const juce::String path = v.getProperty ("path").toString();
            juce::String err;
            Stem::Ptr s = juce::File (path).existsAsFile() ? importStem (juce::File (path), err) : nullptr;
            if (s == nullptr) { missing.add (path); continue; }
            if (path == primaryPath) b->primary = (int) b->stems.size();
            b->stems.push_back (s);
            VoiceFlags fl;
            fl.mute = (bool) v.getProperty ("mute", false);
            fl.solo = (bool) v.getProperty ("solo", false);
            fl.offsetSec = (double) v.getProperty ("offset", 0.0);
            fl.autoOffsetSec = (double) v.getProperty ("autoOffset", 0.0);
            fl.align = (AlignState) juce::jlimit (0, 3, (int) v.getProperty ("align", 0));
            b->flags.push_back (fl);
        }
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
    { const juce::ScopedLock l (pathLock); missingPaths = missing; }
    swapBank (b);
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
