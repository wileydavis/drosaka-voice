// Renders a stem through the full plugin at a series of codec settings, for listening.
//   DrosakaVoiceRender <stem.wav> <out-dir>
// Writes one file per setting plus tour.wav (all of them in order, 0.6 s apart).
#include "../Source/PluginProcessor.h"
#include "../Source/Align.h"
#include <cstdio>

struct Setting { const char* name; bool codec; float bitRate, frameDrop; int lock; };

// Gesture render: DrosakaVoiceRender --build <out.wav> <primary.wav> <other stems...>
// Six loops of the phrase: crowd builds (Count 0 -> all), the codec infects it over four
// loops (Encode 0 -> all, primary last), then the drop: back to the single human voice.
static int renderBuild (int argc, char** argv)
{
    const double rate = 48000.0;
    DrosakaVoiceProcessor p;
    p.prepareToPlay (rate, 512);
    juce::StringArray paths, errs;
    for (int i = 3; i < argc; ++i) paths.add (juce::File (argv[i]).getFullPathName());
    p.addStems (paths, errs);
    if (p.getBank()->size() < 2) { std::printf ("need several stems\n"); return 1; }
    auto set = [&] (const char* id, float v) { auto* prm = p.params.getParameter (id); prm->setValueNotifyingHost (prm->convertTo0to1 (v)); };
    set ("bitRate", 0.45f);
    set ("count", 0.0f);
    set ("encodeCount", 0.0f);

    const auto& st = *p.getBank()->stems[0];
    const int loopLen = (int) (st.audio.getNumSamples() * rate / st.sampleRate) + (int) (0.15 * rate);
    const int loops = 6, total = loopLen * loops + p.getLatencySamples() + (int) rate;
    const int n = p.getBank()->size();
    juce::AudioBuffer<float> out (2, total), block (2, 512);
    for (int pos = 0; pos < total; pos += 512)
    {
        const double loopPos = (double) pos / loopLen;   // 0 .. 6
        float count, encode;
        if (loopPos < 1.0)      { count = (float) loopPos; encode = 0.0f; }
        else if (loopPos < 5.0) { count = 1.0f; encode = (float) ((loopPos - 1.0) / 4.0); }
        else                    { count = 1.0f / (float) n; encode = 0.0f; }   // the drop: one human voice
        set ("count", count);
        set ("encodeCount", juce::jmin (1.0f, encode));
        juce::MidiBuffer midi;
        for (int l = 0; l < loops; ++l)
            if (l * loopLen >= pos && l * loopLen < pos + 512)
                midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), l * loopLen - pos);
        p.processBlock (block, midi);
        const int m = juce::jmin (512, total - pos);
        for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, pos, block, ch, 0, m);
    }
    const juce::File f (argv[2]);
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), rate, 2, 24, {}, 0));
    w->writeFromAudioSampleBuffer (out, p.getLatencySamples(), total - p.getLatencySamples());
    std::printf ("wrote %s: %d voices, %.1f s, peak %.2f\n", f.getFullPathName().toRawUTF8(), n, total / rate, out.getMagnitude (0, total));
    return 0;
}

// Spread tour: DrosakaVoiceRender --spread <out.wav> <primary> <stems...>
// Loops: unison, chorus, chord (minor triad), cluster, a slow sweep 0 -> 1 (Snap 0.5),
// then the chord with the whole crowd encoded (codec plays the full intervals).
static int renderSpread (int argc, char** argv)
{
    const double rate = 48000.0;
    DrosakaVoiceProcessor p;
    p.prepareToPlay (rate, 512);
    juce::StringArray paths, errs;
    for (int i = 3; i < argc; ++i) paths.add (juce::File (argv[i]).getFullPathName());
    p.addStems (paths, errs);
    auto set = [&] (const char* id, float v) { auto* prm = p.params.getParameter (id); prm->setValueNotifyingHost (prm->convertTo0to1 (v)); };
    set ("count", 1.0f); set ("chord", 1.0f); set ("snap", 0.5f); set ("bitRate", 0.45f);

    const auto& st = *p.getBank()->stems[0];
    const int loopLen = (int) (st.audio.getNumSamples() * rate / st.sampleRate) + (int) (0.3 * rate);
    const int loops = 6, total = loopLen * loops + p.getLatencySamples() + (int) rate;
    juce::AudioBuffer<float> out (2, total), block (2, 512);
    for (int pos = 0; pos < total; pos += 512)
    {
        const int loop = juce::jmin (loops - 1, pos / loopLen);
        const float within = (float) (pos - loop * loopLen) / (float) loopLen;
        const float stations[] = { 0.0f, 1.0f / 3.0f, 2.0f / 3.0f, 1.0f };
        set ("spread", loop < 4 ? stations[loop] : (loop == 4 ? within : 2.0f / 3.0f));
        set ("encodeCount", loop == 5 ? 1.0f : 0.0f);
        juce::MidiBuffer midi;
        for (int l = 0; l < loops; ++l)
            if (l * loopLen >= pos && l * loopLen < pos + 512)
                midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), l * loopLen - pos);
        p.processBlock (block, midi);
        const int m = juce::jmin (512, total - pos);
        for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, pos, block, ch, 0, m);
    }
    const juce::File f (argv[2]);
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), rate, 2, 24, {}, 0));
    w->writeFromAudioSampleBuffer (out, p.getLatencySamples(), total - p.getLatencySamples());
    std::printf ("wrote %s, peak %.2f\n", f.getFullPathName().toRawUTF8(), out.getMagnitude (0, total));
    return 0;
}

// Space tour at 120 bpm: DrosakaVoiceRender --space <out.wav> <primary> <stems...>
// Loops: Radiate, Bounce 1/8, Orbit (1 bar per revolution), Swap 1/4, then Bounce 1/16
// with Encode stepping one voice per beat (quantised to 1/4) while the knob ramps.
struct RenderPlayHead : juce::AudioPlayHead
{
    double ppq = 0.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p; p.setBpm (120.0); p.setPpqPosition (ppq); p.setIsPlaying (true); return p;
    }
};

static int renderSpace (int argc, char** argv)
{
    const double rate = 48000.0, spb = rate / 2.0;
    DrosakaVoiceProcessor p;
    p.prepareToPlay (rate, 512);
    RenderPlayHead ph; p.setPlayHead (&ph);
    juce::StringArray paths, errs;
    for (int i = 3; i < argc; ++i) paths.add (juce::File (argv[i]).getFullPathName());
    p.addStems (paths, errs);
    auto set = [&] (const char* id, float v) { auto* prm = p.params.getParameter (id); prm->setValueNotifyingHost (prm->convertTo0to1 (v)); };
    set ("count", 1.0f); set ("bitRate", 0.45f);

    const int loopLen = (int) (spb * 8);   // two bars per loop
    const int loops = 5, total = loopLen * loops + p.getLatencySamples() + (int) rate;
    juce::AudioBuffer<float> out (2, total), block (2, 512);
    for (int pos = 0; pos < total; pos += 512)
    {
        const int loop = juce::jmin (loops - 1, pos / loopLen);
        const float within = (float) (pos - loop * loopLen) / (float) loopLen;
        const float modes[] = { 0, 1, 2, 3, 1 }, divs[] = { 4, 5, 2, 4, 6 };
        set ("spatialMode", modes[loop]);
        set ("motionDiv", divs[loop]);
        set ("encodeQuant", loop == 4 ? 3.0f : 0.0f);
        set ("encodeCount", loop == 4 ? within : 0.0f);
        juce::MidiBuffer midi;
        for (int l = 0; l < loops; ++l)
            if (l * loopLen >= pos && l * loopLen < pos + 512)
                midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), l * loopLen - pos);
        p.processBlock (block, midi);
        ph.ppq += 512.0 / spb;
        const int m = juce::jmin (512, total - pos);
        for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, pos, block, ch, 0, m);
    }
    const juce::File f (argv[2]);
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), rate, 2, 24, {}, 0));
    w->writeFromAudioSampleBuffer (out, p.getLatencySamples(), total - p.getLatencySamples());
    std::printf ("wrote %s, peak %.2f\n", f.getFullPathName().toRawUTF8(), out.getMagnitude (0, total));
    p.setPlayHead (nullptr);
    return 0;
}

// Pattern tour at 120 bpm: DrosakaVoiceRender --pattern <out.wav> <primary> <stems...>
// Slices are placed at the phrase's word gaps, then two bars each of: Walk 1/8, Repeat 1/16,
// Stutter 1/8, Screwed 1/4, Skip/Rewind 1/8, Ratchet (4 bars from 1/4), Random 1/16.
static int renderPattern (int argc, char** argv)
{
    const double rate = 48000.0, spb = rate / 2.0;
    DrosakaVoiceProcessor p;
    p.prepareToPlay (rate, 512);
    RenderPlayHead ph; p.setPlayHead (&ph);
    juce::StringArray paths, errs;
    for (int i = 3; i < argc; ++i) paths.add (juce::File (argv[i]).getFullPathName());
    p.addStems (paths, errs);
    auto set = [&] (const char* id, float v) { auto* prm = p.params.getParameter (id); prm->setValueNotifyingHost (prm->convertTo0to1 (v)); };
    set ("count", 1.0f); set ("spread", 0.15f); set ("playMode", 1.0f); set ("gate", 0.85f);

    // Word gaps: quiet stretches (>= 25 ms) in the primary.
    {
        auto b = p.getBank();
        const auto& st = *b->stems[(size_t) b->primary];
        const int win = (int) (0.005 * st.sampleRate);
        float peak = st.audio.getMagnitude (0, 0, st.audio.getNumSamples());
        int quiet = 0; bool inGap = false; int count = 0;
        for (int i = 0; i + win < st.audio.getNumSamples(); i += win)
        {
            const bool q = st.audio.getRMSLevel (0, i, win) < peak * 0.03f;
            quiet = q ? quiet + 1 : 0;
            if (quiet >= 5) inGap = true;
            if (! q && inGap) { if (i > win * 20) { p.addSlice (i / st.sampleRate - 0.01); ++count; } inGap = false; }
        }
        std::printf ("%d slices at word gaps\n", count);
    }

    struct Section { int type, div, bars; float density; int repeats; };
    const Section sections[] = { { 0, 1, 2, 1, 3 }, { 1, 2, 2, 1, 2 }, { 2, 1, 2, 1, 4 }, { 3, 0, 2, 1, 3 },
                                 { 4, 1, 2, 0.6f, 3 }, { 5, 0, 4, 1, 3 }, { 6, 2, 2, 0.8f, 3 } };
    int totalBars = 0; for (auto& sec : sections) totalBars += sec.bars;
    const int total = (int) (spb * 4 * totalBars) + p.getLatencySamples();
    juce::AudioBuffer<float> out (2, total), block (2, 512);
    for (int pos = 0; pos < total; pos += 512)
    {
        const double bar = (pos / spb) / 4.0;
        int acc = 0; const Section* cur = &sections[0];
        for (auto& sec : sections) { if (bar >= acc) cur = &sec; acc += sec.bars; }
        set ("patternType", (float) cur->type); set ("patternDiv", (float) cur->div);
        set ("density", cur->density); set ("repeats", (float) cur->repeats);
        juce::MidiBuffer midi;
        if (pos == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 48, (juce::uint8) 100), 0);
        p.processBlock (block, midi);
        ph.ppq += 512.0 / spb;
        const int m = juce::jmin (512, total - pos);
        for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, pos, block, ch, 0, m);
    }
    const juce::File f (argv[2]);
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), rate, 2, 24, {}, 0));
    w->writeFromAudioSampleBuffer (out, p.getLatencySamples(), total - p.getLatencySamples());
    std::printf ("wrote %s, peak %.2f\n", f.getFullPathName().toRawUTF8(), out.getMagnitude (0, total));
    p.setPlayHead (nullptr);
    return 0;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc >= 4 && juce::String (argv[1]) == "--build") return renderBuild (argc, argv);
    if (argc >= 4 && juce::String (argv[1]) == "--spread") return renderSpread (argc, argv);
    if (argc >= 4 && juce::String (argv[1]) == "--space") return renderSpace (argc, argv);
    if (argc >= 4 && juce::String (argv[1]) == "--pattern") return renderPattern (argc, argv);
    if (argc >= 4 && juce::String (argv[1]) == "--align")
    {
        // --align ref.wav other.wav ... : print the offset each would get against ref
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        auto env = [&] (const char* path)
        {
            std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (juce::File (juce::File::getCurrentWorkingDirectory().getChildFile (path))));
            juce::AudioBuffer<float> b (1, (int) r->lengthInSamples);
            r->read (&b, 0, (int) r->lengthInSamples, 0, true, false);
            return align::envelope (b.getReadPointer (0), b.getNumSamples(), r->sampleRate);
        };
        auto ref = env (argv[2]);
        for (int i = 3; i < argc; ++i)
        {
            auto res = align::findOffset (ref, env (argv[i]), 0.0);
            std::printf ("%-22s offset %+7.1f ms  score %.2f  margin %.2f  %s\n", argv[i], res.offsetSec * 1000, res.score, res.margin, res.confident ? "confident" : "NOT confident");
        }
        return 0;
    }
    if (argc < 3) { std::printf ("usage: DrosakaVoiceRender <stem> <out-dir>  |  --build <out.wav> <primary> <stems...>\n"); return 1; }
    const double rate = 48000.0;
    const juce::File stem (argv[1]), outDir (argv[2]);
    outDir.createDirectory();

    const Setting settings[] = {
        { "01_dry",                     false, 0.0f,  0.0f, 2 },
        { "02_tracking_clean",          true,  0.0f,  0.0f, 2 },
        { "03_tracking_bitrate35",      true,  0.35f, 0.0f, 2 },
        { "04_tracking_bitrate70",      true,  0.7f,  0.0f, 2 },
        { "05_tracking_bitrate100",     true,  1.0f,  0.0f, 2 },
        { "06_fixed_bitrate35",         true,  0.35f, 0.0f, 0 },
        { "07_free_bitrate35",          true,  0.35f, 0.0f, 1 },
        { "08_tracking_35_drop50",      true,  0.35f, 0.5f, 2 },
        { "09_fixed_100_drop100",       true,  1.0f,  1.0f, 0 },
    };

    juce::AudioBuffer<float> tour (2, 0);
    juce::WavAudioFormat wav;
    auto write = [&] (const juce::File& f, const juce::AudioBuffer<float>& b)
    {
        f.deleteFile();
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), rate, 2, 24, {}, 0));
        w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    };

    for (auto& s : settings)
    {
        DrosakaVoiceProcessor p;
        p.prepareToPlay (rate, 512);
        juce::StringArray errs;
        if (p.addStems ({ stem.getFullPathName() }, errs) != 1) { std::printf ("can't load stem\n"); return 1; }
        auto set = [&] (const char* id, float v) { auto* prm = p.params.getParameter (id); prm->setValueNotifyingHost (prm->convertTo0to1 (v)); };
        set ("encodeCount", s.codec ? 1.0f : 0.0f);
        set ("bitRate", s.bitRate);
        set ("frameDrop", s.frameDrop);
        set ("pitchLock", (float) s.lock);

        const auto& st = *p.getBank()->stems[0];
        const int len = (int) (st.audio.getNumSamples() * rate / st.sampleRate) + p.getLatencySamples() + 4800;
        juce::AudioBuffer<float> out (2, len), block (2, 512);
        juce::AudioBuffer<float> warm (2, 512); juce::MidiBuffer none;
        for (int i = 0; i < 20; ++i) p.processBlock (warm, none);   // let parameter glides settle
        for (int pos = 0; pos < len; pos += 512)
        {
            juce::MidiBuffer midi;
            if (pos == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            p.processBlock (block, midi);
            const int n = juce::jmin (512, len - pos);
            for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, pos, block, ch, 0, n);
        }
        // Drop the latency so every file starts in the same place.
        const int L = p.getLatencySamples();
        juce::AudioBuffer<float> trimmed (2, len - L);
        for (int ch = 0; ch < 2; ++ch) trimmed.copyFrom (ch, 0, out, ch, L, len - L);
        write (outDir.getChildFile (juce::String (s.name) + ".wav"), trimmed);

        const int at = tour.getNumSamples();
        tour.setSize (2, at + trimmed.getNumSamples() + (int) (0.6 * rate), true, true);
        for (int ch = 0; ch < 2; ++ch) tour.copyFrom (ch, at, trimmed, ch, 0, trimmed.getNumSamples());
        std::printf ("%-26s peak %.2f\n", s.name, trimmed.getMagnitude (0, trimmed.getNumSamples()));
    }
    write (outDir.getChildFile ("tour.wav"), tour);
    return 0;
}
