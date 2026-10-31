// Renders a stem through the full plugin at a series of codec settings, for listening.
//   DrosakaVoiceRender <stem.wav> <out-dir>
// Writes one file per setting plus tour.wav (all of them in order, 0.6 s apart).
#include "../Source/PluginProcessor.h"
#include <cstdio>

struct Setting { const char* name; bool codec; float bitRate, frameDrop; int lock; };

int main (int argc, char** argv)
{
    if (argc < 3) { std::printf ("usage: DrosakaVoiceRender <stem> <out-dir>\n"); return 1; }
    juce::ScopedJuceInitialiser_GUI init;
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
        set ("codecTest", s.codec ? 1.0f : 0.0f);
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
