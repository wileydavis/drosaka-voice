// Offline checks of the processor (no host needed). Build with -DDV_BUILD_TESTS=ON.
#include "../Source/PluginProcessor.h"
#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) do { if (cond) std::printf ("  ok   %s\n", msg); \
                              else { std::printf ("  FAIL %s\n", msg); ++failures; } } while (0)

static juce::File writeTestWav (double rate, int len)
{
    // DC-ish ramp 0.5 -> 0.5 so we can see exactly where playback lands.
    auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("dv_test.wav");
    f.deleteFile();
    juce::AudioBuffer<float> b (1, len);
    for (int i = 0; i < len; ++i) b.setSample (0, i, 0.5f);
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), rate, 1, 24, {}, 0));
    w->writeFromAudioSampleBuffer (b, 0, len);
    return f;
}

static juce::AudioBuffer<float> run (DrosakaVoiceProcessor& p, int blockSize, int numBlocks,
                                     std::vector<std::pair<int, int>> notes /* absolute sample, note */)
{
    juce::AudioBuffer<float> out (2, blockSize * numBlocks), block (2, blockSize);
    for (int b = 0; b < numBlocks; ++b)
    {
        juce::MidiBuffer midi;
        for (auto [t, n] : notes)
            if (t >= b * blockSize && t < (b + 1) * blockSize)
                midi.addEvent (juce::MidiMessage::noteOn (1, n, (juce::uint8) 100), t - b * blockSize);
        p.processBlock (block, midi);
        for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, b * blockSize, block, ch, 0, blockSize);
    }
    return out;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    const double rate = 48000.0;

    std::printf ("silence with no stem\n");
    {
        DrosakaVoiceProcessor p; p.prepareToPlay (rate, 256);
        auto out = run (p, 256, 4, { { 10, 60 } });
        CHECK (out.getMagnitude (0, out.getNumSamples()) == 0.0f, "no stem -> silence, no crash");
    }

    const int len = 4800;   // 100 ms
    auto wav = writeTestWav (rate, len);

    std::printf ("trigger timing and one-shot length\n");
    {
        DrosakaVoiceProcessor p; p.prepareToPlay (rate, 256);
        juce::String err;
        CHECK (p.loadStem (wav, err), "loads wav");
        auto out = run (p, 256, 40, { { 300, 60 } });   // note mid-block
        CHECK (out.getMagnitude (0, 0, 300) == 0.0f, "silent before the note");
        CHECK (out.getSample (0, 300) == 0.0f && out.getSample (0, 302) > 0.0f, "starts on the note's sample (with fade-in)");
        CHECK (std::abs (out.getSample (0, 1000) - 0.5f) < 1e-4f && std::abs (out.getSample (1, 1000) - 0.5f) < 1e-4f,
               "mono stem plays at unity on both channels");
        CHECK (out.getSample (0, 300 + len - 300) > 0.4f && out.getSample (0, 300 + len - 3) > 0.0f, "still playing at the end of the stem");
        CHECK (out.getMagnitude (0, 300 + len + 1, 2000) == 0.0f, "silent after it ends (one-shot)");
    }

    std::printf ("pitch is trigger-only\n");
    {
        DrosakaVoiceProcessor a, b; a.prepareToPlay (rate, 256); b.prepareToPlay (rate, 256);
        juce::String err; a.loadStem (wav, err); b.loadStem (wav, err);
        auto oa = run (a, 256, 30, { { 0, 36 } }), ob = run (b, 256, 30, { { 0, 84 } });
        bool same = true;
        for (int i = 0; i < oa.getNumSamples(); ++i) same &= oa.getSample (0, i) == ob.getSample (0, i);
        CHECK (same, "note 36 and note 84 produce identical audio");
    }

    std::printf ("retrigger\n");
    {
        DrosakaVoiceProcessor p; p.prepareToPlay (rate, 256);
        juce::String err; p.loadStem (wav, err);
        auto out = run (p, 256, 40, { { 0, 60 }, { 2000, 60 } });
        float maxJump = 0.0f, peak = 0.0f;
        for (int i = 1; i < out.getNumSamples(); ++i)
        {
            maxJump = juce::jmax (maxJump, std::abs (out.getSample (0, i) - out.getSample (0, i - 1)));
            peak = juce::jmax (peak, std::abs (out.getSample (0, i)));
        }
        CHECK (peak <= 0.5001f, "crossfade never sums above the stem level");
        CHECK (maxJump < 0.02f, "no click on start, retrigger or end");
        CHECK (out.getSample (0, 2000 + len - 300) > 0.4f && out.getMagnitude (0, 2000 + len + 1, 1000) == 0.0f,
               "restarted from the top on the second note");
    }

    std::printf ("sample-rate mismatch\n");
    {
        DrosakaVoiceProcessor p; p.prepareToPlay (96000.0, 256);   // 48k file in a 96k session
        juce::String err; p.loadStem (wav, err);
        auto out = run (p, 256, 60, { { 0, 60 } });
        CHECK (out.getSample (0, len * 2 - 600) > 0.4f && out.getMagnitude (0, len * 2 + 2, 500) == 0.0f,
               "48k stem keeps its real duration at 96k");
    }

    std::printf ("state\n");
    {
        juce::MemoryBlock mb;
        {
            DrosakaVoiceProcessor p; juce::String err; p.loadStem (wav, err);
            p.getStateInformation (mb);
        }
        DrosakaVoiceProcessor q; q.setStateInformation (mb.getData(), (int) mb.getSize());
        CHECK (q.getStem() != nullptr && q.getStem()->file == wav, "reopening the set reloads the stem");

        auto moved = wav.getSiblingFile ("dv_test_moved.wav");
        wav.moveFileTo (moved);
        DrosakaVoiceProcessor r; r.setStateInformation (mb.getData(), (int) mb.getSize());
        CHECK (r.getStem() == nullptr && r.getMissingPath() == wav.getFullPathName(), "moved file is reported missing");
        juce::MemoryBlock mb2; r.getStateInformation (mb2);
        DrosakaVoiceProcessor s2; moved.moveFileTo (wav); s2.setStateInformation (mb2.getData(), (int) mb2.getSize());
        CHECK (s2.getStem() != nullptr, "saving while missing keeps the path");
    }

    std::printf ("bad file\n");
    {
        DrosakaVoiceProcessor p; juce::String err;
        auto junk = wav.getSiblingFile ("junk.wav"); junk.replaceWithText ("not audio");
        CHECK (! p.loadStem (junk, err) && err.isNotEmpty(), "rejects non-audio with a message");
    }

    std::printf (failures == 0 ? "\nALL PASSED\n" : "\n%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
