// Offline checks of the processor (no host needed). Build with -DDV_BUILD_TESTS=ON.
#include "../Source/PluginProcessor.h"
#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) do { if (cond) std::printf ("  ok   %s\n", msg); \
                              else { std::printf ("  FAIL %s\n", msg); ++failures; } } while (0)

static const juce::File tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("dv_tests");

static juce::File writeWav (const juce::String& name, const juce::AudioBuffer<float>& b, double rate)
{
    tmp.createDirectory();
    auto f = tmp.getChildFile (name);
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), rate,
                                                                     (unsigned) b.getNumChannels(), 24, {}, 0));
    w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    return f;
}

static juce::File constWav (const juce::String& name, float value, int len, double rate = 48000.0)
{
    juce::AudioBuffer<float> b (1, len);
    for (int i = 0; i < len; ++i) b.setSample (0, i, value);
    return writeWav (name, b, rate);
}

// Harmonic "voice": f0 with harmonics rolled off; 'bright' tilts energy upward.
static juce::AudioBuffer<float> voiceTone (float f0, float bright, double rate, int len)
{
    juce::AudioBuffer<float> b (1, len);
    b.clear();
    for (int h = 1; h * f0 < 7000.0f; ++h)
    {
        const float amp = std::pow ((float) h, bright - 1.5f) * 0.1f;
        for (int i = 0; i < len; ++i)
            b.addSample (0, i, amp * std::sin (juce::MathConstants<float>::twoPi * f0 * (float) h * (float) i / (float) rate));
    }
    return b;
}

static juce::AudioBuffer<float> run (DrosakaVoiceProcessor& p, int blockSize, int numBlocks,
                                     std::vector<int> noteTimes, std::function<void (int)> perBlock = {})
{
    juce::AudioBuffer<float> out (2, blockSize * numBlocks), block (2, blockSize);
    for (int b = 0; b < numBlocks; ++b)
    {
        if (perBlock) perBlock (b);
        juce::MidiBuffer midi;
        for (int t : noteTimes)
            if (t >= b * blockSize && t < (b + 1) * blockSize)
                midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), t - b * blockSize);
        p.processBlock (block, midi);
        for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, b * blockSize, block, ch, 0, blockSize);
    }
    return out;
}

static void setCount (DrosakaVoiceProcessor& p, float v)
{
    p.params.getParameter ("count")->setValueNotifyingHost (v);
}

static float maxJump (const juce::AudioBuffer<float>& b)
{
    float m = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 1; i < b.getNumSamples(); ++i)
            m = juce::jmax (m, std::abs (b.getSample (ch, i) - b.getSample (ch, i - 1)));
    return m;
}

// Build a bank directly for crowd-math tests.
static Bank::Ptr fakeBank (std::vector<float> pitches, int primary)
{
    Bank::Ptr b = new Bank();
    for (auto hz : pitches)
    {
        Stem::Ptr s = new Stem();
        s->analysis.pitchHz = hz;
        s->analysis.centroidHz = 3000.0f - hz;   // inverse, to test the formant axis
        b->stems.push_back (s);
        b->flags.push_back ({});
    }
    b->primary = primary;
    b->resort();
    return b;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    const double rate = 48000.0;

    std::printf ("analysis\n");
    {
        auto lo = voiceTone (110.0f, 0.0f, 44100.0, 44100);
        auto hi = voiceTone (220.0f, 0.0f, 44100.0, 44100);
        auto bright = voiceTone (110.0f, 1.2f, 44100.0, 44100);
        auto a = analyseStem (lo.getReadPointer (0), lo.getNumSamples(), 44100.0);
        auto b = analyseStem (hi.getReadPointer (0), hi.getNumSamples(), 44100.0);
        auto c = analyseStem (bright.getReadPointer (0), bright.getNumSamples(), 44100.0);
        std::printf ("       110 Hz tone -> %.1f Hz, centroid %.0f Hz\n", a.pitchHz, a.centroidHz);
        std::printf ("       220 Hz tone -> %.1f Hz, centroid %.0f Hz\n", b.pitchHz, b.centroidHz);
        std::printf ("       bright 110  -> %.1f Hz, centroid %.0f Hz\n", c.pitchHz, c.centroidHz);
        CHECK (std::abs (a.pitchHz - 110.0f) < 2.0f, "110 Hz pitch within 2 Hz");
        CHECK (std::abs (b.pitchHz - 220.0f) < 3.0f, "220 Hz pitch within 3 Hz");
        CHECK (std::abs (c.pitchHz - 110.0f) < 2.0f, "bright tone keeps its pitch");
        CHECK (c.centroidHz > a.centroidHz * 1.3f, "brighter tone has a higher formant centroid");

        juce::AudioBuffer<float> silence (1, 44100); silence.clear();
        auto s = analyseStem (silence.getReadPointer (0), 44100, 44100.0);
        CHECK (s.pitchHz == 0.0f, "silence -> no pitch, no crash");
    }

    std::printf ("sorting\n");
    {
        auto b = fakeBank ({ 220, 110, 330 }, 0);
        CHECK (b->sorted == std::vector<int> ({ 1, 0, 2 }), "pitch axis sorts low -> high");
        b->axis = SortAxis::formant; b->resort();
        CHECK (b->sorted == std::vector<int> ({ 2, 0, 1 }), "formant axis sorts by centroid");
    }

    std::printf ("crowd math (5 voices, primary in the middle)\n");
    {
        auto b = fakeBank ({ 100, 200, 300, 400, 500 }, 2);   // sorted == import order
        float g[maxVoices], p[maxVoices];

        computeCrowd (*b, 0.0f, g, p);
        CHECK (g[0] + g[1] + g[2] + g[3] + g[4] == 0.0f, "Count 0 -> silence");

        computeCrowd (*b, 1.0f, g, p);
        CHECK (g[2] == 1.0f && g[1] == 0.0f && g[3] == 0.0f && p[2] == 0.0f, "Count 1 -> primary alone, centred");

        computeCrowd (*b, 2.0f, g, p);
        CHECK (g[1] > 0.0f && std::abs (g[1] - g[3]) < 1e-6f && g[0] == 0.0f, "Count 2 -> first pair half-way in, together");

        computeCrowd (*b, 3.0f, g, p);
        CHECK (g[1] > 0.0f && g[0] == 0.0f && p[1] < 0.0f && p[3] > 0.0f && std::abs (p[1] + p[3]) < 1e-6f,
               "Count 3 -> pair either side, lower voice left, symmetric");
        CHECK (std::abs (g[1] * g[1] + g[2] * g[2] + g[3] * g[3] - 1.0f) < 1e-4f, "level normalised (power = 1)");

        computeCrowd (*b, 5.0f, g, p);
        CHECK (std::abs (p[0]) > std::abs (p[1]) && p[0] < 0.0f && p[4] > 0.0f, "outer pair pans wider");
        bool allEqual = true;
        for (int i = 1; i < 5; ++i) allEqual &= std::abs (g[i] - g[0]) < 1e-6f;
        CHECK (allEqual, "Count all -> every voice equal");

        // Continuity: no jumps as the knob sweeps.
        float prev[5]; computeCrowd (*b, 0.0f, prev, p);
        float worst = 0.0f;
        for (int k = 1; k <= 500; ++k)
        {
            computeCrowd (*b, 5.0f * (float) k / 500.0f, g, p);
            for (int i = 0; i < 5; ++i) worst = juce::jmax (worst, std::abs (g[i] - prev[i]));
            std::copy (g, g + 5, prev);
        }
        CHECK (worst < 0.05f, "Count sweep is continuous");
    }

    std::printf ("crowd math (edges, even size, mute, solo)\n");
    {
        auto b = fakeBank ({ 100, 200, 300, 400 }, 0);   // primary at the low edge
        float g[maxVoices], p[maxVoices];
        computeCrowd (*b, 3.0f, g, p);
        CHECK (g[0] > 0.0f && g[1] > 0.0f && g[2] > 0.0f && g[3] == 0.0f, "primary at the edge: window grows to one side");
        computeCrowd (*b, 4.0f, g, p);
        CHECK (g[3] > 0.0f && std::abs (g[3] - g[0]) < 1e-6f, "even-size bank reaches every voice at full Count");

        b->flags[1].mute = true;
        computeCrowd (*b, 4.0f, g, p);
        CHECK (g[1] == 0.0f && g[2] > 0.0f, "mute silences one voice, keeps its slot");
        b->flags[1].mute = false;

        b->flags[3].solo = true;
        computeCrowd (*b, 1.0f, g, p);
        CHECK (g[3] > 0.0f && g[0] == 0.0f, "solo auditions a voice outside the Count window");
    }

    // Engine tests: three constant-level stems so we can read the mix directly.
    const int len = 9600;   // 200 ms
    auto fA = constWav ("a.wav", 0.10f, len), fB = constWav ("b.wav", 0.20f, len), fC = constWav ("c.wav", 0.30f, len);

    std::printf ("engine\n");
    {
        DrosakaVoiceProcessor p; p.prepareToPlay (rate, 256);
        juce::StringArray errs;
        CHECK (p.addStems ({ fA.getFullPathName(), fB.getFullPathName(), fC.getFullPathName() }, errs) == 3, "imports three stems");
        auto bank = p.getBank();
        CHECK (bank->primary == 0, "first stem becomes primary");

        setCount (p, 1.0f / 3.0f);   // primary only
        run (p, 256, 20, {});        // let the crowd settle
        auto out = run (p, 256, 40, { 300 });
        CHECK (out.getMagnitude (0, 0, 300) == 0.0f, "silent before the note");
        CHECK (std::abs (out.getSample (0, 2000) - 0.10f) < 1e-3f && std::abs (out.getSample (1, 2000) - 0.10f) < 1e-3f,
               "Count = 1 voice: only the primary, centred, at unity");

        setCount (p, 1.0f);
        run (p, 256, 20, {});
        out = run (p, 256, 40, { 300 });
        const float l = out.getSample (0, 2000), r = out.getSample (1, 2000);
        CHECK (l > 0.10f && r > 0.10f && std::abs (l - r) > 1e-3f, "full Count: whole crowd, spread across the stereo field");
        CHECK (out.getSample (0, 300) == 0.0f && out.getSample (0, 302) > 0.0f, "every voice starts on the note's sample");
        CHECK (maxJump (out) < 0.03f, "no clicks with the crowd playing");

        // Turn Count down mid-phrase: glides, never steps.
        setCount (p, 1.0f);
        run (p, 256, 20, {});
        out = run (p, 256, 40, { 0 }, [&] (int blk) { if (blk == 10) setCount (p, 0.0f); });
        CHECK (maxJump (out) < 0.03f, "Count change mid-phrase crossfades smoothly");
        CHECK (out.getMagnitude (0, 256 * 20, 2000) < 1e-4f, "Count 0 fades to silence within ~50 ms");

        p.setPrimary (2);
        p.setMute (1, true);
        juce::MemoryBlock mb; p.getStateInformation (mb);
        DrosakaVoiceProcessor q; q.setStateInformation (mb.getData(), (int) mb.getSize());
        auto qb = q.getBank();
        CHECK (qb->size() == 3 && qb->stems[(size_t) qb->primary]->file == fC && qb->flags[1].mute,
               "set reopens with bank, primary and mute intact");
        CHECK (std::abs (q.params.getParameter ("count")->getValue()) < 1e-6f, "Count knob value restored");

        auto moved = fB.getSiblingFile ("b_moved.wav");
        fB.moveFileTo (moved);
        DrosakaVoiceProcessor r2; r2.setStateInformation (mb.getData(), (int) mb.getSize());
        CHECK (r2.getBank()->size() == 2 && r2.getMissingPaths().size() == 1, "moved stem reported missing, rest load");
        juce::MemoryBlock mb2; r2.getStateInformation (mb2);
        moved.moveFileTo (fB);
        DrosakaVoiceProcessor r3; r3.setStateInformation (mb2.getData(), (int) mb2.getSize());
        CHECK (r3.getBank()->size() == 3, "saving while missing keeps the path");

        p.removeStem (0);
        CHECK (p.getBank()->size() == 2 && p.getBank()->stems[(size_t) p.getBank()->primary]->file == fC,
               "removing a voice keeps the primary");
    }

    std::printf ("milestone 1 sets still open\n");
    {
        juce::ValueTree old ("DrosakaVoice");
        old.setProperty ("version", 1, nullptr);
        old.setProperty ("stemPath", fA.getFullPathName(), nullptr);
        juce::MemoryBlock mb;
        juce::AudioProcessor::copyXmlToBinary (*old.createXml(), mb);
        DrosakaVoiceProcessor p; p.setStateInformation (mb.getData(), (int) mb.getSize());
        CHECK (p.getBank()->size() == 1, "M1 state loads as a one-voice bank");
    }

    std::printf ("bad input\n");
    {
        DrosakaVoiceProcessor p; juce::StringArray errs;
        auto junk = tmp.getChildFile ("junk.wav"); junk.replaceWithText ("not audio");
        CHECK (p.addStems ({ junk.getFullPathName() }, errs) == 0 && errs.size() == 1, "rejects non-audio with a message");
        p.prepareToPlay (rate, 256);
        auto out = run (p, 256, 4, { 10 });
        CHECK (out.getMagnitude (0, out.getNumSamples()) == 0.0f, "empty bank -> silence, no crash");
    }

    tmp.deleteRecursively();
    std::printf (failures == 0 ? "\nALL PASSED\n" : "\n%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
