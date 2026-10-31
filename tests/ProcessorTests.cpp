// Offline checks of the processor (no host needed). Build with -DDV_BUILD_TESTS=ON.
#include "../Source/PluginProcessor.h"
#include <cstdio>
#include <algorithm>

static int failures = 0;

// A stand-in for Live's transport.
struct TestPlayHead : juce::AudioPlayHead
{
    double ppq = 0.0, bpm = 120.0;
    bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setBpm (bpm);
        p.setPpqPosition (ppq);
        p.setIsPlaying (playing);
        return p;
    }
};
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

        const int L = p.getLatencySamples();
        CHECK (L == LpcCodec::latencySamples (rate) && L > 0, "reports the codec latency to the host");

        setCount (p, 1.0f / 3.0f);   // primary only
        run (p, 256, 20, {});        // let the crowd settle
        auto out = run (p, 256, 40, { 300 });
        CHECK (out.getMagnitude (0, 0, 300 + L) == 0.0f, "silent before the note (+ latency)");
        CHECK (std::abs (out.getSample (0, 2000 + L) - 0.10f) < 1e-3f && std::abs (out.getSample (1, 2000 + L) - 0.10f) < 1e-3f,
               "Count = 1 voice: only the primary, centred, at unity");

        setCount (p, 1.0f);
        run (p, 256, 20, {});
        out = run (p, 256, 40, { 300 });
        const float l = out.getSample (0, 2000 + L), r = out.getSample (1, 2000 + L);
        CHECK (l > 0.10f && r > 0.10f && std::abs (l - r) > 1e-3f, "full Count: whole crowd, spread across the stereo field");
        CHECK (out.getSample (0, 300 + L) == 0.0f && out.getSample (0, 302 + L) > 0.0f,
               "every voice starts exactly one latency after the note (host compensates)");
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
        CHECK (r2.getBank()->size() == 3 && r2.getMissingPaths().isEmpty(), "moved stem still loads: its audio is embedded in the set");
        juce::MemoryBlock mb2; r2.getStateInformation (mb2);
        moved.moveFileTo (fB);
        DrosakaVoiceProcessor r3; r3.setStateInformation (mb2.getData(), (int) mb2.getSize());
        CHECK (r3.getBank()->size() == 3, "saving while missing keeps the path");

        p.removeStem (0);
        CHECK (p.getBank()->size() == 2 && p.getBank()->stems[(size_t) p.getBank()->primary]->file == fC,
               "removing a voice keeps the primary");
    }


    std::printf ("codec (unit)\n");
    {
        const int n = (int) rate * 3;
        std::vector<float> x ((size_t) n), y ((size_t) n);
        auto tone = voiceTone (150.0f, 0.3f, rate, n);
        for (int i = 0; i < n; ++i)
        {
            const double t = i / rate, syl = std::fmod (t, 0.27);
            const float env = (t > 0.2 && syl < 0.18) ? (float) std::sin (juce::MathConstants<double>::pi * syl / 0.18) : 0.0f;
            x[(size_t) i] = tone.getSample (0, i) * env;
        }
        auto envOf = [] (const std::vector<float>& v)
        {
            std::vector<float> e; float s = 0.0f; const float a = std::exp (-1.0f / (0.004f * 48000.0f));
            for (size_t i = 0; i < v.size(); ++i) { s = a * s + (1 - a) * std::abs (v[i]); if (i % 48 == 0) e.push_back (s); }
            return e;
        };
        const auto ex = envOf (x);
        double px = 0; for (auto v : x) px += v * v;

        bool allFinite = true, allBounded = true, levelOk = true, alignOk = true;
        for (int lock = 0; lock < 3; ++lock)
            for (float br : { 0.0f, 0.35f, 0.7f, 1.0f })
            {
                LpcCodec c; c.prepare (rate, 1); c.setParams (br, 0.0f, (LpcCodec::PitchLock) lock, 100.0f);
                c.process (x.data(), y.data(), n);
                double py = 0; for (auto v : y) { py += v * v; allFinite &= std::isfinite (v); allBounded &= std::abs (v) < 2.0f; }
                const double db = 10.0 * std::log10 (py / px);
                if (lock == 2) std::printf ("       tracking, bit rate %.2f: %+.1f dB\n", br, db);
                if (std::abs (db) >= 3.0) std::printf ("       lock %d bit rate %.2f: level %+.1f dB\n", lock, br, db);
                levelOk &= std::abs (db) < 3.0;
                if (br < 0.5f)
                {
                    const auto ey = envOf (y);
                    const int expected = juce::roundToInt (LpcCodec::latencySamples (rate) / 48.0);
                    int bestLag = 0; double best = -1;
                    for (int lag = 0; lag < 100; ++lag)
                    {
                        double acc = 0; for (size_t i = 0; i + (size_t) lag < ey.size(); ++i) acc += ex[i] * ey[i + (size_t) lag];
                        if (acc > best) { best = acc; bestLag = lag; }
                    }
                    if (std::abs (bestLag - expected) > 3) std::printf ("       lock %d bit rate %.2f: lag %d ms, expected %d\n", lock, br, bestLag, expected);
                    alignOk &= std::abs (bestLag - expected) <= 3;
                }
            }
        CHECK (allFinite && allBounded, "every lock mode and bit rate: finite, bounded output");
        CHECK (levelOk, "level within 3 dB of the input at every bit rate");
        CHECK (alignOk, "codec output lines up with the latency-delayed dry path (within 3 ms)");

        auto pitchOf = [&] (LpcCodec::PitchLock lock, float fixedHz)
        {
            LpcCodec c; c.prepare (rate, 1); c.setParams (0.2f, 0.0f, lock, fixedHz);
            c.process (tone.getReadPointer (0), y.data(), n);
            return analyseStem (y.data() + 9600, n - 9600, rate).pitchHz;
        };
        CHECK (std::abs (pitchOf (LpcCodec::PitchLock::tracking, 100.0f) - 150.0f) < 4.0f, "Tracking follows the voice's pitch");
        CHECK (std::abs (pitchOf (LpcCodec::PitchLock::fixed, 100.0f) - 100.0f) < 3.0f, "Fixed holds the set pitch");

        // Frame drop: gaps appear, and only when asked for.
        auto silentFrames = [&] (float drop)
        {
            LpcCodec c; c.prepare (rate, 1); c.setParams (0.2f, drop, LpcCodec::PitchLock::tracking, 100.0f);
            c.process (tone.getReadPointer (0), y.data(), n);
            int quiet = 0, total = 0;
            for (int f = 9600; f + 1080 < n; f += 1080, ++total)
            {
                double e = 0; for (int i = 0; i < 1080; ++i) e += y[(size_t) (f + i)] * y[(size_t) (f + i)];
                if (std::sqrt (e / 1080) < 0.005) ++quiet;
            }
            return (float) quiet / (float) total;
        };
        CHECK (silentFrames (0.0f) == 0.0f, "no dropouts at frame drop 0");
        CHECK (silentFrames (1.0f) > 0.08f, "frame drop 100% produces audible dropouts");

        // Silence and noise.
        // No ringing after the voice stops, even at the coarsest settings.
        {
            LpcCodec c; c.prepare (rate, 1); c.setParams (1.0f, 0.0f, LpcCodec::PitchLock::tracking, 100.0f);
            std::vector<float> burst ((size_t) n, 0.0f);
            for (int i = 0; i < (int) rate; ++i) burst[(size_t) i] = tone.getSample (0, i);
            c.process (burst.data(), y.data(), n);
            float tail = 0; for (int i = (int) rate + 9600; i < n; ++i) tail = juce::jmax (tail, std::abs (y[(size_t) i]));
            CHECK (tail < 1e-3f, "no ringing after the voice stops (coarsest bit rate)");
        }

        std::vector<float> z ((size_t) n, 0.0f);
        LpcCodec c; c.prepare (rate, 1); c.setParams (1.0f, 1.0f, LpcCodec::PitchLock::free, 100.0f);
        c.process (z.data(), y.data(), n);
        float mag = 0; for (auto v : y) mag = juce::jmax (mag, std::abs (v));
        CHECK (mag < 1e-4f, "silence in -> silence out");
        juce::Random r (3); for (auto& v : z) v = r.nextFloat() * 2.0f - 1.0f;
        c.process (z.data(), y.data(), n);
        bool ok = true; for (auto v : y) ok &= std::isfinite (v) && std::abs (v) < 3.0f;
        CHECK (ok, "full-scale noise at the harshest settings stays finite and bounded");
    }

    std::printf ("codec in the engine\n");
    {
        auto toneFile = writeWav ("tone.wav", voiceTone (150.0f, 0.3f, rate, (int) rate), rate);
        DrosakaVoiceProcessor p; p.prepareToPlay (rate, 256);
        juce::StringArray errs; p.addStems ({ toneFile.getFullPathName() }, errs);
        auto dry = run (p, 256, 120, { 0 });
        p.params.getParameter ("encodeCount")->setValueNotifyingHost (1.0f);
        run (p, 256, 10, {});
        auto enc = run (p, 256, 120, { 0 });
        const int a0 = 9600, len2 = 24000;
        double pd = 0, pe = 0, cross = 0;
        for (int i = a0; i < a0 + len2; ++i)
        {
            pd += dry.getSample (0, i) * dry.getSample (0, i);
            pe += enc.getSample (0, i) * enc.getSample (0, i);
            cross += dry.getSample (0, i) * enc.getSample (0, i);
        }
        CHECK (std::abs (10.0 * std::log10 (pe / pd)) < 3.0, "codec on primary: level within 3 dB of dry");
        CHECK (cross / std::sqrt (pd * pe) < 0.9, "codec on primary: audibly different waveform from dry");

        // Flip it on and off mid-phrase: crossfades, no clicks.
        p.params.getParameter ("encodeCount")->setValueNotifyingHost (0.0f);
        run (p, 256, 10, {});
        auto sw = run (p, 256, 120, { 0 }, [&] (int blk)
        {
            if (blk == 40) p.params.getParameter ("encodeCount")->setValueNotifyingHost (1.0f);
            if (blk == 80) p.params.getParameter ("encodeCount")->setValueNotifyingHost (0.0f);
        });
        // A click shows up as a step bigger than anything the dry or codec signal makes on its own.
        const float natural = juce::jmax (maxJump (dry), maxJump (enc));
        std::printf ("       max step: dry/codec %.4f, while switching %.4f\n", natural, maxJump (sw));
        CHECK (maxJump (sw) < natural * 1.25f, "switching the codec mid-phrase crossfades without clicks");
    }


    std::printf ("encode count\n");
    {
        auto b = fakeBank ({ 100, 200, 300, 400, 500, 600, 700, 800, 900 }, 4);   // primary in the middle
        const auto& o = b->encodeOrder;
        CHECK (o.size() == 9 && o.back() == 4, "every voice in the order, primary last");
        auto sorted = o; std::sort (sorted.begin(), sorted.end());
        bool perm = true; for (int i = 0; i < 9; ++i) perm &= sorted[(size_t) i] == i;
        CHECK (perm, "order is a permutation of the bank");
        bool alternates = true;
        for (size_t i = 1; i + 1 < o.size(); ++i) alternates &= (o[i] < 4) != (o[i - 1] < 4);
        CHECK (alternates, "infection alternates left/right of the primary (jumps around the head)");
        auto again = fakeBank ({ 100, 200, 300, 400, 500, 600, 700, 800, 900 }, 4);
        CHECK (again->encodeOrder == o, "fixed seed: same bank, same order");
        bool notSequential = false;
        for (size_t i = 1; i + 1 < o.size(); ++i) notSequential |= std::abs (o[i] - o[i - 1]) > 2;
        CHECK (notSequential, "order is scattered, not a sweep through the array");

        auto edge = fakeBank ({ 100, 200, 300, 400 }, 0);   // primary at the edge: one side only
        CHECK (edge->encodeOrder.size() == 4 && edge->encodeOrder.back() == 0, "primary last even at the array's edge");

        float e[maxVoices], all[maxVoices];
        std::fill (all, all + maxVoices, 1.0f);
        computeEncode (*b, 0.0f / 9, all, e);
        CHECK (*std::max_element (e, e + 9) == 0.0f, "Encode 0 -> all dry");
        computeEncode (*b, 1.0f / 9, all, e);
        CHECK (e[o[0]] == 1.0f && std::count (e, e + 9, 0.0f) == 8, "Encode 1 -> exactly one voice, at full strength");
        computeEncode (*b, 2.5f / 9, all, e);
        CHECK (e[o[0]] == 1.0f && e[o[1]] == 1.0f && std::abs (e[o[2]] - 0.5f) < 1e-6f, "voices cross over one at a time");
        computeEncode (*b, 8.0f / 9, all, e);
        CHECK (e[4] == 0.0f && std::count (e, e + 9, 1.0f) == 8, "Encode all-but-one -> only the primary still human");
        computeEncode (*b, 9.0f / 9, all, e);
        CHECK (std::count (e, e + 9, 1.0f) == 9, "Encode all -> the primary falls last");

        // Only audible voices count: Count window of 3 around the primary (sorted 3,4,5).
        float g[maxVoices], pn[maxVoices]; int rk[maxVoices];
        computeCrowd (*b, 3.0f, g, pn);
        computeEncode (*b, 1.0f / 3.0f, g, e, rk);
        CHECK (std::count (e, e + 9, 1.0f) == 1 && (e[3] == 1.0f || e[5] == 1.0f), "Encode 1/3 with 3 audible -> one audible voice, at full strength");
        CHECK (e[0] + e[1] + e[2] + e[6] + e[7] + e[8] == 0.0f, "silent voices are never spent on");
        CHECK (rk[4] == 3 && rk[0] == 0 && rk[3] + rk[5] == 3, "ranks count audible voices only, primary last");
        computeEncode (*b, 2.0f / 3.0f, g, e);
        CHECK (e[3] == 1.0f && e[5] == 1.0f && e[4] == 0.0f, "Encode 2/3 -> both neighbours, primary still human");
        computeEncode (*b, 1.0f, g, e);
        CHECK (std::count (e, e + 9, 1.0f) == 9, "Encode 100% -> everything, so voices fading in arrive encoded");
        b->flags[3].mute = true;
        computeCrowd (*b, 3.0f, g, pn);
        computeEncode (*b, 0.5f, g, e);
        CHECK (e[5] == 1.0f && e[3] == 0.0f && e[4] == 0.0f, "muted voices drop out of the count");
        b->flags[3].mute = false;

        b->primary = 7; b->resort();
        CHECK (b->encodeOrder.back() == 7, "changing the primary re-plans the order with it last");
    }

    std::printf ("encode count in the engine\n");
    {
        juce::StringArray paths;
        const float f0s[] = { 110, 130, 150, 175, 200 };
        for (int i = 0; i < 5; ++i)
            paths.add (writeWav ("v" + juce::String (i) + ".wav", voiceTone (f0s[i], 0.3f, rate, (int) rate * 2), rate).getFullPathName());
        DrosakaVoiceProcessor p; p.prepareToPlay (rate, 256);
        juce::StringArray errs; p.addStems (paths, errs);
        setCount (p, 1.0f);
        auto enc = [&] (float v) { p.params.getParameter ("encodeCount")->setValueNotifyingHost (v); };
        enc (0.0f); run (p, 256, 10, {});
        auto dry = run (p, 256, 300, { 0 });
        enc (1.0f); run (p, 256, 10, {});
        auto full = run (p, 256, 300, { 0 });
        enc (0.0f); run (p, 256, 10, {});
        auto sweep = run (p, 256, 300, { 0 }, [&] (int blk) { enc (juce::jlimit (0.0f, 1.0f, (float) (blk - 30) / 240.0f)); });
        const float natural = juce::jmax (maxJump (dry), maxJump (full));
        std::printf ("       max step: dry/encoded %.4f, sweeping Encode %.4f\n", natural, maxJump (sweep));
        CHECK (maxJump (sweep) < natural * 1.25f, "sweeping Encode across the crowd: no clicks");
        auto rms = [] (const juce::AudioBuffer<float>& b2, int a, int n) { return b2.getRMSLevel (0, a, n) + b2.getRMSLevel (1, a, n); };
        const double ratio = 20.0 * std::log10 (rms (full, 20000, 40000) / rms (dry, 20000, 40000));
        std::printf ("       fully encoded vs dry crowd level: %+.1f dB\n", ratio);
        {
            // Coherence: crowd power vs the sum of each voice's power, dry and encoded.
            auto pw = [] (const juce::AudioBuffer<float>& b2) { double a = 0; for (int i = 20000; i < 60000; ++i) a += b2.getSample (0, i) * b2.getSample (0, i); return a; };
            double sd = 0, se = 0;
            for (int v = 0; v < 5; ++v)
            {
                p.setSolo (v, true);
                enc (0.0f); run (p, 256, 10, {}); sd += pw (run (p, 256, 300, { 0 }));
                enc (1.0f); run (p, 256, 10, {}); se += pw (run (p, 256, 300, { 0 }));
                p.setSolo (v, false);
            }
            std::printf ("       dry: crowd/sum-of-solos %+.1f dB   encoded: %+.1f dB\n", 10 * std::log10 (pw (dry) * 5 / sd), 10 * std::log10 (pw (full) * 5 / se));
            CHECK (std::abs (10 * std::log10 (pw (full) * 5 / se)) < 1.0, "encoded voices add like independent voices (no shared DC)");
        }
        CHECK (std::abs (ratio) < 3.0, "fully encoded crowd within 3 dB of the dry crowd");
    }


    std::printf ("alignment\n");
    {
        // Syllable patterns: same words = same pattern; different words = different pattern.
        auto speechish = [&] (float f0, float bright, std::vector<std::pair<float, float>> syllables, double padSec)
        {
            const int pad = (int) (padSec * rate), len = pad + (int) (rate * 2.2);
            auto tone = voiceTone (f0, bright, rate, len);
            juce::AudioBuffer<float> b (1, len); b.clear();
            for (auto [start, dur] : syllables)
                for (int i = 0; i < (int) (dur * rate); ++i)
                {
                    const int at = pad + (int) (start * rate) + i;
                    if (at < len) b.setSample (0, at, tone.getSample (0, at) * std::sin (juce::MathConstants<float>::pi * (float) i / (dur * (float) rate)));
                }
            return b;
        };
        const std::vector<std::pair<float, float>> wordsA { { 0.10f, 0.14f }, { 0.30f, 0.22f }, { 0.58f, 0.09f }, { 0.72f, 0.18f }, { 1.05f, 0.25f }, { 1.40f, 0.11f }, { 1.58f, 0.30f } };
        const std::vector<std::pair<float, float>> wordsB { { 0.05f, 0.25f }, { 0.40f, 0.10f }, { 0.55f, 0.30f }, { 0.95f, 0.12f }, { 1.15f, 0.12f }, { 1.35f, 0.35f } };

        auto fRef  = writeWav ("al_ref.wav",  speechish (120.0f, 0.3f, wordsA, 0.0), rate);
        auto fTake = writeWav ("al_take.wav", speechish (210.0f, 1.0f, wordsA, 0.317), rate);   // other voice, 317 ms late
        auto fLead = writeWav ("al_lead.wav", speechish (160.0f, 0.6f, wordsA, 0.0), rate);
        auto fDiff = writeWav ("al_diff.wav", speechish (150.0f, 0.5f, wordsB, 0.2), rate);     // different words

        DrosakaVoiceProcessor p; p.prepareToPlay (rate, 256);
        juce::StringArray errs;
        p.addStems ({ fRef.getFullPathName(), fTake.getFullPathName(), fLead.getFullPathName(), fDiff.getFullPathName() }, errs);
        auto b = p.getBank();
        const auto& f = b->flags;
        std::printf ("       take offset %.1f ms (%d), lead %.1f ms (%d), different words %.1f ms (%d)\n",
                     f[1].offsetSec * 1000, (int) f[1].align, f[2].offsetSec * 1000, (int) f[2].align, f[3].offsetSec * 1000, (int) f[3].align);
        CHECK (f[0].align == AlignState::reference, "primary is the alignment reference");
        CHECK (f[1].align == AlignState::autoAligned && std::abs (f[1].offsetSec + 0.317) < 0.004, "a late take in another voice is pulled back 317 ms (within 4 ms)");
        CHECK (f[2].align == AlignState::autoAligned && std::abs (f[2].offsetSec) < 0.004, "an already-aligned voice stays put");
        CHECK (f[3].align == AlignState::unsure && f[3].offsetSec == 0.0, "different words: flagged, not moved");

        // Playback honours offsets: the take's first syllable now lands with the primary's.
        auto onsetOf = [&] (int voice)
        {
            p.setSolo (voice, true);
            run (p, 256, 700, {});   // let any previous phrase (2.5 s) finish
            auto out = run (p, 256, 120, { 0 });
            p.setSolo (voice, false);
            // Both channels: the crowd is panned, so a voice may sit hard left or right.
            for (int i = 0; i < out.getNumSamples(); ++i)
                if (std::abs (out.getSample (0, i)) + std::abs (out.getSample (1, i)) > 0.01f) return i;
            return -1;
        };
        const int o0 = onsetOf (0), o1 = onsetOf (1);
        std::printf ("       onsets: primary %d, aligned take %d (samples)\n", o0, o1);
        CHECK (o0 > 0 && std::abs (o0 - o1) < (int) (0.006 * rate), "aligned take plays in sync with the primary");

        // Manual nudge, reset, and negative offsets that start mid-file.
        p.setOffset (3, -0.2);
        CHECK (p.getBank()->flags[3].offsetSec == -0.2, "manual drag sets the offset");
        p.setOffset (1, 0.05);
        CHECK (p.getBank()->flags[1].align == AlignState::manual, "dragging an auto-aligned voice marks it manual");
        p.resetOffset (1);
        CHECK (std::abs (p.getBank()->flags[1].offsetSec + 0.317) < 0.004 && p.getBank()->flags[1].align == AlignState::autoAligned,
               "double-click reset returns to the auto position");
        const int o3 = onsetOf (3);
        CHECK (o3 >= 0, "negative offset (starts mid-file) plays without trouble");

        // Saved with the set.
        juce::MemoryBlock mb; p.getStateInformation (mb);
        DrosakaVoiceProcessor q; q.setStateInformation (mb.getData(), (int) mb.getSize());
        CHECK (std::abs (q.getBank()->flags[1].offsetSec + 0.317) < 0.004 && q.getBank()->flags[3].offsetSec == -0.2
               && q.getBank()->flags[3].align == AlignState::unsure, "offsets and alignment state reopen with the set");

        // Align all after moving things around.
        p.setOffset (1, 1.0);
        p.alignAll();
        CHECK (std::abs (p.getBank()->flags[1].offsetSec + 0.317) < 0.004 && p.getBank()->flags[3].offsetSec == -0.2,
               "Align all re-aligns what it can and leaves flagged voices where you put them");
    }


    std::printf ("spread (paths)\n");
    {
        auto b = fakeBank ({ 100, 200, 300, 400, 500, 600, 700 }, 3);   // primary in the middle
        float c[maxVoices], d[maxVoices];
        spread::compute (*b, 0.0f, 1, 0.0f, c, d);
        CHECK (*std::max_element (c, c + 7) == 0.0f && *std::min_element (c, c + 7) == 0.0f, "Spread 0 -> unison");
        spread::compute (*b, 1.0f / 3.0f, 1, 0.0f, c, d);
        CHECK (c[3] == 0.0f && c[2] < 0.0f && c[4] > 0.0f && std::abs (c[2]) < 0.3f && std::abs (c[0]) > std::abs (c[2]),
               "chorus station: small detune, opposite sides, wider further out");
        spread::compute (*b, 2.0f / 3.0f, 1, 0.0f, c, d);   // minor triad
        CHECK (c[4] == 3.0f && c[2] == -5.0f && c[5] == 7.0f && c[1] == -9.0f && c[6] == 12.0f && c[0] == -12.0f,
               "chord station: minor triad tones, outer voices widest");
        CHECK (d[6] == 0.0f && d[0] == 0.0f && d[5] == 7.0f, "dry voices fold wide tones by octaves into +-7 st");
        spread::compute (*b, 1.0f, 1, 0.0f, c, d);
        CHECK (c[4] == 1.0f && c[5] == 2.0f && c[6] == 3.0f && c[2] == -1.0f && c[0] == -3.0f, "cluster station: a semitone apart");
        CHECK (c[3] == 0.0f, "primary never moves");

        // Continuity of both paths across the whole knob, at several Snap settings.
        float worst = 0.0f;
        for (float snap : { 0.0f, 0.5f, 1.0f })
        {
            float pc[maxVoices], pd[maxVoices];
            spread::compute (*b, 0.0f, 1, snap, pc, pd);
            for (int k = 1; k <= 3000; ++k)
            {
                spread::compute (*b, (float) k / 3000.0f, 1, snap, c, d);
                for (int i = 0; i < 7; ++i) worst = juce::jmax (worst, std::abs (c[i] - pc[i]), std::abs (d[i] - pd[i]));
                std::copy (c, c + 7, pc); std::copy (d, d + 7, pd);
            }
        }
        std::printf ("       largest step between adjacent knob positions: %.3f st\n", worst);
        CHECK (worst < 0.25f, "every voice glides, nothing steps (even at full Snap)");

        // Snap: lingers near stations.
        spread::compute (*b, 2.0f / 3.0f - 0.08f, 1, 1.0f, c, d);
        const float nearChordSnap = c[6];
        spread::compute (*b, 2.0f / 3.0f - 0.08f, 1, 0.0f, c, d);
        CHECK (std::abs (nearChordSnap - 12.0f) < std::abs (c[6] - 12.0f) * 0.3f, "full Snap lingers on the chord tone");
    }

    std::printf ("spread (audio)\n");
    {
        // Syllabic harmonic tones, so PSOLA has real pitch periods and gaps to keep in time.
        auto syllabic = [&] (float f0)
        {
            const int len = (int) (rate * 2.0);
            auto t = voiceTone (f0, 0.4f, rate, len);
            for (int i = 0; i < len; ++i)
            {
                const double tt = i / rate, ph = std::fmod (tt, 0.4);
                t.setSample (0, i, t.getSample (0, i) * (tt > 0.1 && ph < 0.28 ? (float) std::sin (juce::MathConstants<double>::pi * ph / 0.28) : 0.0f));
            }
            return t;
        };
        auto fPrim = writeWav ("sp_prim.wav", syllabic (150.0f), rate);
        auto fUp = writeWav ("sp_up.wav", syllabic (200.0f), rate);
        DrosakaVoiceProcessor p; p.prepareToPlay (rate, 256);
        juce::StringArray errs; p.addStems ({ fPrim.getFullPathName(), fUp.getFullPathName() }, errs);
        auto set = [&] (const char* id, float v) { auto* prm = p.params.getParameter (id); prm->setValueNotifyingHost (prm->convertTo0to1 (v)); };
        set ("snap", 0.0f);
        p.setSolo (1, true);

        auto render = [&] (float spreadKnob, int chord)
        {
            set ("spread", spreadKnob); set ("chord", (float) chord);
            run (p, 256, 500, {});   // settle and let any phrase finish
            auto out = run (p, 256, 400, { 0 });
            juce::AudioBuffer<float> mono (1, out.getNumSamples());
            for (int i = 0; i < out.getNumSamples(); ++i) mono.setSample (0, i, out.getSample (0, i) + out.getSample (1, i));
            return mono;
        };
        auto pitchOf = [&] (const juce::AudioBuffer<float>& m) { return analyseStem (m.getReadPointer (0), m.getNumSamples(), rate).pitchHz; };
        auto envLag = [&] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& c2)
        {
            auto env = [&] (const juce::AudioBuffer<float>& m) { std::vector<float> e; float sm = 0; for (int i = 0; i < m.getNumSamples(); ++i) { sm = 0.995f * sm + 0.005f * std::abs (m.getSample (0, i)); if (i % 48 == 0) e.push_back (sm); } return e; };
            auto ea = env (a), ec = env (c2);
            int best = 0; double bv = -1;
            for (int lag = -30; lag <= 30; ++lag)
            {
                double acc = 0; for (int i = 40; i + 40 < (int) ea.size(); ++i) acc += ea[(size_t) i] * ec[(size_t) (i + lag)];
                if (acc > bv) { bv = acc; best = lag; }
            }
            return best;   // ms
        };

        auto dry = render (0.0f, 0);
        auto fifth = render (2.0f / 3.0f, 0);   // Fifths: the right-hand voice goes up 7
        auto minor = render (2.0f / 3.0f, 1);   // Minor triad: up 3
        auto clus = render (1.0f, 0);           // cluster: up 1
        const float pd = pitchOf (dry), pf = pitchOf (fifth), pm = pitchOf (minor), pc = pitchOf (clus);
        std::printf ("       dry %.1f Hz, fifth %.1f (want %.1f), minor 3rd %.1f (want %.1f), cluster %.1f (want %.1f)\n",
                     pd, pf, 200 * std::exp2 (7 / 12.0), pm, 200 * std::exp2 (3 / 12.0), pc, 200 * std::exp2 (1 / 12.0));
        CHECK (std::abs (pf - 200 * std::exp2 (7 / 12.0)) < 4.0, "PSOLA: up a fifth lands on the fifth");
        CHECK (std::abs (pm - 200 * std::exp2 (3 / 12.0)) < 4.0, "PSOLA: up a minor third lands on it");
        CHECK (std::abs (pc - 200 * std::exp2 (1 / 12.0)) < 3.0, "PSOLA: cluster semitone lands on it");

        const int lag = envLag (dry, fifth);
        std::printf ("       timing shift after pitching up a fifth: %d ms\n", lag);
        CHECK (std::abs (lag) <= 5, "pitch shifting keeps the timing (syllables stay put)");
        const double db = 20 * std::log10 (fifth.getRMSLevel (0, 0, fifth.getNumSamples()) / dry.getRMSLevel (0, 0, dry.getNumSamples()));
        std::printf ("       level after pitching up a fifth: %+.1f dB\n", db);
        CHECK (std::abs (db) < 2.5, "pitch shifting keeps the level within 2.5 dB");

        // Codec: the full interval, past what PSOLA allows. Octaves set: +12 for this voice.
        set ("encodeCount", 1.0f);
        auto oct = render (2.0f / 3.0f, 5);
        const float po = pitchOf (oct);
        std::printf ("       encoded voice, octave up: %.1f Hz (want ~400)\n", po);
        CHECK (std::abs (po - 400.0f) < 10.0f, "encoded voices play the full interval (an octave, beyond the dry +-7)");
        set ("encodeCount", 0.0f);

        // A slow sweep of the whole knob while the phrase plays: no clicks, no level holes.
        set ("spread", 0.0f);
        run (p, 256, 500, {});
        auto sweep = run (p, 256, 400, { 0 }, [&] (int blk) { set ("spread", juce::jlimit (0.0f, 1.0f, (float) blk / 380.0f)); });
        auto stillOut = run (p, 256, 10, {});
        set ("spread", 2.0f / 3.0f); run (p, 256, 500, {});
        auto steadyShift = run (p, 256, 400, { 0 });
        set ("spread", 0.0f); run (p, 256, 500, {});
        auto steadyDry = run (p, 256, 400, { 0 });
        const float natural = juce::jmax (maxJump (steadyDry), maxJump (steadyShift));
        std::printf ("       max step: steady %.4f, sweeping Spread %.4f\n", natural, maxJump (sweep));
        CHECK (maxJump (sweep) < natural * 1.3f, "sweeping Spread mid-phrase: no clicks");
        bool finite = true; for (int i = 0; i < sweep.getNumSamples(); ++i) finite &= std::isfinite (sweep.getSample (0, i));
        CHECK (finite, "output stays finite through the sweep");
        p.setSolo (1, false);
    }


    std::printf ("spatial modes (maths)\n");
    {
        auto b = fakeBank ({ 100, 200, 300, 400, 500, 600, 700 }, 3);
        float g[maxVoices], rad[maxVoices], out[maxVoices], out2[maxVoices];
        computeCrowd (*b, 7.0f, g, rad);
        spatial::pans (spatial::radiate, *b, rad, g, 0, 0.0, out);
        CHECK (std::equal (out, out + 7, rad), "Radiate = the crowd layout");

        spatial::pans (spatial::swap, *b, rad, g, 1, 0.0, out);
        bool mirrored = true; for (int i = 0; i < 7; ++i) mirrored &= std::abs (out[i] + rad[i]) < 1e-6f;
        CHECK (mirrored && out[3] == 0.0f, "Swap mirrors the field on odd divisions, primary stays centred");

        spatial::pans (spatial::bounce, *b, rad, g, 10, 0.0, out);
        spatial::pans (spatial::bounce, *b, rad, g, 11, 0.0, out2);
        int moved = 0; float lo = 1, hi = -1;
        for (int i = 0; i < 7; ++i) { if (i != 3 && std::abs (out[i] - out2[i]) > 0.01f) ++moved; lo = juce::jmin (lo, out[i]); hi = juce::jmax (hi, out[i]); }
        CHECK (moved >= 4 && out[3] == 0.0f && out2[3] == 0.0f, "Bounce: voices hop each division, primary stays centred");
        CHECK (lo <= -0.99f && hi >= 0.99f, "Bounce keeps the crowd spread edge to edge");
        float again[maxVoices]; spatial::pans (spatial::bounce, *b, rad, g, 10, 0.0, again);
        CHECK (std::equal (out, out + 7, again), "Bounce is repeatable: same bar, same positions (fixed seed)");

        spatial::pans (spatial::orbit, *b, rad, g, 0, 0.25, out);
        spatial::pans (spatial::orbit, *b, rad, g, 0, 0.25 + 1.0, out2);
        bool periodic = true; for (int i = 0; i < 7; ++i) periodic &= std::abs (out[i] - out2[i]) < 1e-4f;
        CHECK (periodic && out[3] == 0.0f, "Orbit: one revolution per division, primary centred");

        float half[maxVoices]; computeCrowd (*b, 3.0f, half, rad);
        spatial::pans (spatial::bounce, *b, rad, half, 5, 0.0, out);
        CHECK (out[2] != rad[2] || out[4] != rad[4], "Bounce moves only the audible voices around");
    }

    std::printf ("tempo sync\n");
    {
        juce::StringArray paths;
        const float f0s[] = { 120, 140, 160, 180, 200 };
        for (int i = 0; i < 5; ++i)
            paths.add (writeWav ("ts" + juce::String (i) + ".wav", voiceTone (f0s[i], 0.3f, rate, (int) rate * 6), rate).getFullPathName());
        DrosakaVoiceProcessor p; p.prepareToPlay (rate, 256);
        TestPlayHead ph; p.setPlayHead (&ph);
        juce::StringArray errs; p.addStems (paths, errs);
        auto set = [&] (const char* id, float v) { auto* prm = p.params.getParameter (id); prm->setValueNotifyingHost (prm->convertTo0to1 (v)); };
        set ("count", 1.0f);
        const double spb = rate * 60.0 / ph.bpm;            // 24000 samples per beat at 120
        const double latBeats = p.getLatencySamples() / spb;

        // Drive the transport block by block.
        auto block = [&] (int nBlocks, std::function<void (int)> each = {})
        {
            juce::AudioBuffer<float> buf (2, 256);
            for (int k = 0; k < nBlocks; ++k)
            {
                if (each) each (k);
                juce::MidiBuffer m;
                p.processBlock (buf, m);
                if (ph.playing) ph.ppq += 256.0 / spb;
            }
        };
        auto encodedCount = [&] { int c = 0; for (int s = 0; s < 5; ++s) c += p.getVoiceEncode (s) > 0.5f ? 1 : 0; return c; };

        // Encode quantised to 1/4: turn the knob to 60% (3 of 5 voices) mid-beat;
        // nothing flips until the next beat line, as heard (i.e. latency-compensated).
        set ("encodeQuant", 3.0f);   // 1/4
        set ("encodeCount", 0.0f);
        ph.ppq = 10.25 + latBeats;   // the output sample is at beat 10.25
        block (20);
        set ("encodeCount", 0.6f);
        const double toNextBeat = 11.0 - (ph.ppq - latBeats);   // beats until the line, as heard
        const int blocksBefore = (int) (toNextBeat * spb / 256.0) - 2;
        block (blocksBefore);
        const int before = encodedCount();
        block (14);   // past the line + the 30 ms crossfade
        const int after = encodedCount();
        std::printf ("       encoded voices: %d just before the beat, %d after it\n", before, after);
        CHECK (before == 0 && after == 3, "quantised Encode waits for the beat, then flips the whole step");

        set ("encodeQuant", 0.0f);   // Free
        set ("encodeCount", 0.2f);
        block (12);
        CHECK (encodedCount() == 1, "Free Encode follows the knob immediately");

        // Bounce on 1/8: pans change only on the grid lines, and land on them.
        set ("encodeCount", 0.0f);
        set ("spatialMode", 1.0f); set ("motionDiv", 5.0f);   // Bounce, 1/8
        ph.ppq = 20.0 + latBeats + 0.01;
        block (8);
        std::vector<float> trace;
        block (400, [&] (int) { trace.push_back (p.getVoicePan (4)); });   // ~4.3 beats, 256-sample resolution
        int changes = 0;
        for (size_t i = 1; i < trace.size(); ++i) if (std::abs (trace[i] - trace[i - 1]) > 0.05f && (i < 2 || std::abs (trace[i - 1] - trace[i - 2]) < 0.05f)) ++changes;
        std::printf ("       Bounce at 1/8 over ~4.3 beats: %d hops\n", changes);
        CHECK (changes >= 7 && changes <= 9, "Bounce hops once per 1/8 note");

        // Stopped transport: an internal clock keeps motion going at the last tempo.
        ph.playing = false;
        trace.clear();
        block (400, [&] (int) { trace.push_back (p.getVoicePan (4)); });
        changes = 0;
        for (size_t i = 1; i < trace.size(); ++i) if (std::abs (trace[i] - trace[i - 1]) > 0.05f && (i < 2 || std::abs (trace[i - 1] - trace[i - 2]) < 0.05f)) ++changes;
        CHECK (changes >= 7 && changes <= 9, "with Live stopped, motion keeps time on the internal clock");
        ph.playing = true;

        // Orbit: smooth, no hops; no clicks in the audio.
        set ("spatialMode", 2.0f); set ("motionDiv", 4.0f);   // Orbit, one revolution per beat
        juce::AudioBuffer<float> orbitOut (2, 256 * 300), tmp (2, 256);
        for (int k = 0; k < 300; ++k)
        {
            juce::MidiBuffer m; if (k == 0) m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            p.processBlock (tmp, m); ph.ppq += 256.0 / spb;
            for (int ch = 0; ch < 2; ++ch) orbitOut.copyFrom (ch, k * 256, tmp, ch, 0, 256);
        }
        set ("spatialMode", 1.0f); set ("motionDiv", 6.0f);   // Bounce at 1/16: the busiest hopping
        juce::AudioBuffer<float> bounceOut (2, 256 * 300);
        for (int k = 0; k < 300; ++k)
        {
            juce::MidiBuffer m; if (k == 0) m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            p.processBlock (tmp, m); ph.ppq += 256.0 / spb;
            for (int ch = 0; ch < 2; ++ch) bounceOut.copyFrom (ch, k * 256, tmp, ch, 0, 256);
        }
        set ("spatialMode", 0.0f);
        juce::AudioBuffer<float> still (2, 256 * 300);
        for (int k = 0; k < 300; ++k)
        {
            juce::MidiBuffer m; if (k == 0) m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            p.processBlock (tmp, m); ph.ppq += 256.0 / spb;
            for (int ch = 0; ch < 2; ++ch) still.copyFrom (ch, k * 256, tmp, ch, 0, 256);
        }
        std::printf ("       max step: radiate %.4f, orbit %.4f, bounce 1/16 %.4f\n", maxJump (still), maxJump (orbitOut), maxJump (bounceOut));
        CHECK (maxJump (orbitOut) < maxJump (still) * 1.3f && maxJump (bounceOut) < maxJump (still) * 1.3f, "moving voices around: no clicks");
        p.setPlayHead (nullptr);
    }


    std::printf ("slicing\n");
    {
        auto toneA = voiceTone (140.0f, 0.4f, rate, (int) (rate * 2.0));
        auto toneB = voiceTone (190.0f, 0.4f, rate, (int) (rate * 2.0));
        auto fA = writeWav ("sl_a.wav", toneA, rate), fB = writeWav ("sl_b.wav", toneB, rate);
        DrosakaVoiceProcessor p; p.prepareToPlay (rate, 256);
        juce::StringArray errs; p.addStems ({ fA.getFullPathName(), fB.getFullPathName() }, errs);
        const int L = p.getLatencySamples();

        p.addSlice (1.0); p.addSlice (0.5); p.addSlice (1.5);
        p.addSlice (1.004);   // too close to an existing marker
        auto b = p.getBank();
        CHECK (b->slices == std::vector<double> ({ 0.5, 1.0, 1.5 }) && b->numRegions() == 4, "markers stay sorted; near-duplicates ignored");
        p.moveSlice (1, 0.2);   // can't cross its left neighbour
        CHECK (std::abs (p.getBank()->slices[1] - 0.51) < 1e-9, "a dragged marker stops at its neighbour");
        p.moveSlice (1, 1.0);

        auto play = [&] (int note)
        {
            run (p, 256, 500, {});   // let anything ringing finish
            juce::AudioBuffer<float> out (2, 256 * 450), blk (2, 256);
            for (int k = 0; k < 450; ++k)
            {
                juce::MidiBuffer m;
                if (k == 0) m.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), 0);
                p.processBlock (blk, m);
                for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, k * 256, blk, ch, 0, 256);
            }
            return out;
        };
        auto mag = [] (const juce::AudioBuffer<float>& o, int a, int n2) { return juce::jmax (o.getMagnitude (0, a, n2), o.getMagnitude (1, a, n2)); };

        auto whole = play (40);   // not a slice key: the whole phrase
        auto s2 = play (61);      // C#3: region 2 = 0.5 .. 1.0 s
        const int r0 = L + (int) (0.5 * rate), r1 = L + (int) (1.0 * rate);
        CHECK (mag (whole, L + (int) (1.7 * rate), 2000) > 0.01f, "other keys play the whole phrase");
        CHECK (mag (s2, L + 200, (int) (0.5 * rate) - 400) > 0.01f, "slice key plays its region straight away");
        CHECK (mag (s2, L + (int) (0.5 * rate) + 10, (int) (0.4 * rate)) < 1e-4f, "and stops at the region's end marker");

        // Same audio as the phrase at that point (both voices: one marker cut every voice).
        double num = 0, da = 0, db = 0;
        for (int i = 2000; i < (int) (0.45 * rate); ++i)
            for (int ch = 0; ch < 2; ++ch)
            {
                const float x = s2.getSample (ch, L + i), y = whole.getSample (ch, r0 + i);
                num += x * y; da += x * x; db += y * y;
            }
        std::printf ("       slice vs the same span of the whole phrase: correlation %.4f\n", num / std::sqrt (da * db));
        CHECK (num / std::sqrt (da * db) > 0.99, "the slice is that span of the phrase, across every voice");
        (void) r1;

        auto last = play (63);    // D#3: region 4 = 1.5 s .. end
        CHECK (mag (last, L + 200, (int) (0.4 * rate)) > 0.01f && mag (last, L + (int) (0.6 * rate), 4000) < 1e-4f, "last slice runs to the end of the phrase");

        p.removeSlice (0); p.removeSlice (0); p.removeSlice (0);
        CHECK (p.getBank()->slices.empty() && p.getBank()->numRegions() == 0, "removing every marker: back to whole-phrase playback");
    }

    std::printf ("presets with embedded audio\n");
    {
        juce::StringArray paths;
        std::vector<juce::AudioBuffer<float>> originals;
        for (int i = 0; i < 3; ++i)
        {
            originals.push_back (voiceTone (120.0f + 30.0f * (float) i, 0.4f, 44100.0, 44100 * 2));
            paths.add (writeWav ("emb" + juce::String (i) + ".wav", originals.back(), 44100.0).getFullPathName());
        }
        DrosakaVoiceProcessor p; p.prepareToPlay (rate, 256);
        juce::StringArray errs; p.addStems (paths, errs);
        p.setPrimary (1); p.setMute (2, true); p.setOffset (0, 0.123);
        p.addSlice (0.7); p.addSlice (1.3);
        auto set = [&] (DrosakaVoiceProcessor& q, const char* id, float v) { auto* prm = q.params.getParameter (id); prm->setValueNotifyingHost (prm->convertTo0to1 (v)); };
        set (p, "spread", 0.4f); set (p, "spatialMode", 2.0f); set (p, "encodeCount", 0.5f);

        juce::MemoryBlock mb; p.getStateInformation (mb);
        double rawBytes = 0; for (auto& o : originals) rawBytes += o.getNumSamples() * 3.0;
        std::printf ("       state: %.0f KB for %.0f KB of 24-bit audio\n", mb.getSize() / 1024.0, rawBytes / 1024.0);

        // The source files go away (another machine, a moved folder...).
        for (auto& path : paths) juce::File (path).moveFileTo (juce::File (path).withFileExtension ("gone"));

        DrosakaVoiceProcessor q; q.prepareToPlay (rate, 256);
        q.setStateInformation (mb.getData(), (int) mb.getSize());
        auto qb = q.getBank();
        CHECK (qb->size() == 3 && q.getMissingPaths().isEmpty(), "set reopens with every stem though the files are gone");
        float worst = 0.0f;
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < originals[(size_t) i].getNumSamples(); ++k)
                worst = juce::jmax (worst, std::abs (qb->stems[(size_t) i]->audio.getSample (0, k) - originals[(size_t) i].getSample (0, k)));
        std::printf ("       largest sample difference after the round trip: %.2e\n", worst);
        CHECK (worst < 1e-5f, "embedded audio is lossless (24-bit)");
        CHECK (qb->stems[1]->sampleRate == 44100.0, "keeps each stem's own sample rate");
        CHECK (qb->primary == 1 && qb->flags[2].mute && std::abs (qb->flags[0].offsetSec - 0.123) < 1e-9,
               "primary, mute and alignment offsets come back");
        CHECK (qb->slices == std::vector<double> ({ 0.7, 1.3 }), "slice markers come back");
        CHECK (std::abs (q.params.getParameter ("spread")->getValue() - 0.4f) < 1e-4f && (int) q.params.getRawParameterValue ("spatialMode")->load() == 2,
               "knob states come back");
        CHECK (std::abs (qb->stems[0]->analysis.pitchHz - 120.0f) < 3.0f && ! qb->stems[0]->marks.pos.empty(), "analysis rebuilt from the embedded audio");

        auto presetFile = tmp.getChildFile ("bank.dvoice");
        CHECK (q.savePreset (presetFile) && presetFile.getSize() > 1000, "Save preset writes a file");
        DrosakaVoiceProcessor r; r.prepareToPlay (rate, 256);
        CHECK (r.loadPreset (presetFile) && r.getBank()->size() == 3 && r.getBank()->slices.size() == 2, "Load preset brings the whole bank back");
        auto out = run (r, 256, 100, { 0 });
        CHECK (out.getMagnitude (0, out.getNumSamples()) > 0.01f, "a loaded preset plays");

        // A milestone 2-6 set (XML, file paths) still opens.
        for (auto& path : paths) juce::File (path).withFileExtension ("gone").moveFileTo (juce::File (path));
        juce::ValueTree old ("DrosakaVoice");
        old.setProperty ("version", 3, nullptr);
        juce::ValueTree bt ("Bank");
        bt.setProperty ("primary", paths[2], nullptr);
        for (auto& path : paths) { juce::ValueTree v ("Stem"); v.setProperty ("path", path, nullptr); bt.appendChild (v, nullptr); }
        old.appendChild (bt, nullptr);
        juce::MemoryBlock omb; juce::AudioProcessor::copyXmlToBinary (*old.createXml(), omb);
        DrosakaVoiceProcessor o; o.setStateInformation (omb.getData(), (int) omb.getSize());
        CHECK (o.getBank()->size() == 3 && o.getBank()->primary == 2, "older sets (file paths) still open");
        juce::MemoryBlock resaved; o.getStateInformation (resaved);
        CHECK (resaved.getSize() > 50000, "and are saved with their audio embedded from then on");
    }


    std::printf ("patterns (steps)\n");
    {
        auto runSteps = [] (pattern::Params pp, int regions, int steps, std::vector<std::vector<pattern::Hit>>& out, pattern::State st = {})
        {
            double ppq = st.startPpq;
            for (int k = 0; k < steps; ++k)
            {
                pattern::Hit h[8]; int nh = 0;
                const double len = pattern::nextStep (st, pp, regions, ppq, h, nh);
                out.emplace_back (h, h + nh);
                ppq += len;
            }
            return st;
        };
        auto slicesOf = [] (const std::vector<std::vector<pattern::Hit>>& v)
        {
            std::vector<int> r; for (auto& step : v) r.push_back (step.empty() ? -1 : step[0].slice); return r;
        };
        pattern::Params pp; pp.division = 0.5;
        std::vector<std::vector<pattern::Hit>> v;

        pp.type = pattern::walk; runSteps (pp, 4, 6, v);
        CHECK (slicesOf (v) == std::vector<int> ({ 0, 1, 2, 3, 0, 1 }), "Walk: one slice per step, in order, looping");

        v.clear(); pp.type = pattern::repeat; pp.repeats = 3; runSteps (pp, 3, 7, v);
        CHECK (slicesOf (v) == std::vector<int> ({ 0, 0, 0, 1, 1, 1, 2 }), "Repeat: each slice N times, then on");

        v.clear(); pp.type = pattern::stutter; pp.repeats = 4; runSteps (pp, 3, 3, v);
        CHECK (v[0].size() == 1 && v[1].size() == 4 && v[1][3].offset == 0.375 && v[1][0].length == 0.125 && v[1][0].slice == 0 && v[2][0].slice == 1,
               "Stutter: the slice, then N shorter retriggers of it, then on");

        v.clear(); pp.type = pattern::screwed; pp.drop = 7.0f; runSteps (pp, 2, 2, v);
        CHECK (v[0][0].pitch == -7.0f && v[0][0].fit && v[1][0].slice == 1, "Screwed: pitched down by Drop, fitted to the step");

        v.clear(); pp.type = pattern::skipRewind; pp.density = 0.0f; runSteps (pp, 4, 8, v);
        CHECK (slicesOf (v) == std::vector<int> ({ 0, 1, 2, 3, 0, 1, 2, 3 }), "Skip/Rewind at density 0 = Walk");
        v.clear(); pp.density = 1.0f; runSteps (pp, 4, 400, v);
        int silent = 0, back = 0; int prev = -1;
        for (size_t i = 0; i < v.size(); ++i)
        {
            if (v[i].empty()) ++silent;
            else { if (prev >= 0 && v[i][0].slice == prev) ++back; prev = v[i][0].slice; }
        }
        std::printf ("       Skip/Rewind at full density over 400 steps: %d silent, %d rewinds\n", silent, back);
        CHECK (silent > 150 && silent < 250 && back > 50, "Skip/Rewind at full density: about half the steps silent, the rest stepping back");
        std::vector<std::vector<pattern::Hit>> v2; runSteps (pp, 4, 400, v2);
        CHECK (slicesOf (v) == slicesOf (v2), "Skip/Rewind is repeatable (same pattern every time)");

        v.clear(); pp.type = pattern::ratchet; pp.division = 0.5; runSteps (pp, 4, 30, v);
        std::vector<double> lens; for (auto& step : v) lens.push_back (step[0].length);
        CHECK (lens[0] == 0.5 && lens[7] == 0.5 && lens[8] == 0.25 && lens[23] == 0.25 && lens[24] == 0.125,
               "Ratchet: the division halves every bar, accelerating");

        v.clear(); pp.type = pattern::random; pp.density = 1.0f; runSteps (pp, 5, 15, v);
        bool perms = true, differs = false;
        for (int c = 0; c < 3; ++c)
        {
            std::vector<int> cyc; for (int i = 0; i < 5; ++i) cyc.push_back (v[(size_t) (c * 5 + i)][0].slice);
            auto sorted = cyc; std::sort (sorted.begin(), sorted.end());
            perms &= sorted == std::vector<int> ({ 0, 1, 2, 3, 4 });
            if (c > 0) for (int i = 0; i < 5; ++i) differs |= v[(size_t) (c * 5 + i)][0].slice != v[(size_t) i][0].slice;
        }
        CHECK (perms && differs, "Random: every slice once per pass, a new order each pass");
        v.clear(); pp.density = 0.5f; runSteps (pp, 5, 400, v);
        int fired = 0; for (auto& step : v) fired += step.empty() ? 0 : 1;
        CHECK (fired > 160 && fired < 240, "Random density 50%: about half the steps fire");

        v.clear(); pp.type = pattern::walk;
        pattern::State st; st.slice = 0;
        pattern::Hit h[8]; int nh = 0;
        pattern::nextStep (st, pp, 6, 0.0, h, nh);
        st.jump = 4;
        pattern::nextStep (st, pp, 6, 0.5, h, nh);
        const int jumped = h[0].slice;
        pattern::nextStep (st, pp, 6, 1.0, h, nh);
        CHECK (jumped == 4 && h[0].slice == 5, "a held slice key: the walk jumps there on the next step, then carries on");
    }

    std::printf ("patterns (engine)\n");
    {
        // Two aligned voices; four slices of 0.4 s.
        auto tA = voiceTone (140.0f, 0.4f, rate, (int) (rate * 1.6)), tB = voiceTone (180.0f, 0.4f, rate, (int) (rate * 1.6));
        auto fA = writeWav ("pt_a.wav", tA, rate), fB = writeWav ("pt_b.wav", tB, rate);
        DrosakaVoiceProcessor p; p.prepareToPlay (rate, 256);
        TestPlayHead ph; ph.bpm = 120.0; ph.ppq = 0.0; p.setPlayHead (&ph);
        juce::StringArray errs; p.addStems ({ fA.getFullPathName(), fB.getFullPathName() }, errs);
        p.addSlice (0.4); p.addSlice (0.8); p.addSlice (1.2);
        auto set = [&] (const char* id, float v) { auto* prm = p.params.getParameter (id); prm->setValueNotifyingHost (prm->convertTo0to1 (v)); };
        set ("playMode", 1.0f); set ("patternType", 0.0f); set ("patternDiv", 1.0f); set ("gate", 0.5f);   // Walk, 1/8, half-length hits
        const double spb = rate * 60.0 / ph.bpm;
        const int L = p.getLatencySamples();

        // Play: note events at given blocks; record output.
        auto perform = [&] (int blocks, std::vector<std::tuple<int, int, bool>> events)   // block, note, on?
        {
            juce::AudioBuffer<float> out (2, 256 * blocks), blk (2, 256);
            for (int k = 0; k < blocks; ++k)
            {
                juce::MidiBuffer m;
                for (auto& [bk, note, on] : events)
                    if (bk == k) m.addEvent (on ? juce::MidiMessage::noteOn (1, note, (juce::uint8) 100) : juce::MidiMessage::noteOff (1, note), 37);
                p.processBlock (blk, m);
                ph.ppq += 256.0 / spb;
                for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, k * 256, blk, ch, 0, 256);
            }
            return out;
        };
        auto energy = [] (const juce::AudioBuffer<float>& o, int a, int n2)
        {
            if (a < 0 || a + n2 > o.getNumSamples()) return 0.0f;
            return o.getRMSLevel (0, a, n2) + o.getRMSLevel (1, a, n2);
        };

        // Key down at beat 0.1 (block ~9): the walk starts on the next 1/8 line (beat 0.5).
        const double startPpq = ph.ppq;
        auto out = perform (600, { { 9, 48, true }, { 400, 48, false } });
        const double pressPpq = startPpq + (9 * 256 + 37) / spb;
        const double first = std::ceil (pressPpq / 0.5) * 0.5;
        int onTime = 0, total = 0;
        for (int k = 0; k < 8; ++k)   // the 8 lines before the key is released at block 400
        {
            const int at = (int) std::round ((first + 0.5 * k - startPpq) * spb) + L;   // heard L later (Live compensates)
            ++total;
            if (energy (out, at + 100, 2400) > 0.02f && energy (out, at - 3000, 2400) < 0.002f) ++onTime;
        }
        std::printf ("       Walk 1/8: %d of %d hits start exactly on their grid line\n", onTime, total);
        CHECK (onTime == total, "hits land on the 1/8 grid, with silence between them (Gate 50%)");
        const int releaseAt = 400 * 256 + 37;
        CHECK (energy (out, releaseAt + L + (int) (0.5 * spb) + 2000, 20000) < 1e-3f, "releasing the key stops the walk");

        // Latch: keeps going after release.
        set ("latch", 1.0f);
        auto latched = perform (600, { { 5, 48, true }, { 60, 48, false } });
        CHECK (energy (latched, 400 * 256, 24000) > 0.02f, "Latch: the walk keeps running after the key is released");
        set ("latch", 0.0f);
        perform (60, {});
        CHECK (! p.patternActive.load(), "turning Latch off stops it");

        // A slice key starts the walk from that slice, and holding one mid-walk jumps there.
        perform (200, { { 3, 62, true }, { 10, 62, false } });   // starts at slice 2 (D3)
        perform (100, {});
        perform (300, { { 3, 48, true }, { 100, 63, true }, { 101, 63, false }, { 290, 48, false } });
        // (the jump is checked through the slice that plays right after the press)
        auto jumpRun = perform (400, { { 3, 48, true }, { 150, 62, true } });
        const int sliceAfterJump = p.patternSlice.load();
        perform (20, { { 0, 62, false }, { 0, 48, false } });
        CHECK (sliceAfterJump == 2 || sliceAfterJump == 3, "holding a slice key mid-walk jumps the walk to it");

        // Screwed: the same slices, pitched down by Drop and fitted to the step.
        set ("patternType", 3.0f); set ("dropSemis", 7.0f); set ("gate", 1.0f); set ("patternDiv", 0.0f);   // 1/4 steps: 0.5 s
        p.setSolo (0, true);   // one voice, so the pitch is measurable
        auto scr = perform (500, { { 5, 48, true } });
        perform (20, { { 0, 48, false } });
        juce::AudioBuffer<float> mono (1, 24000);
        for (int i = 0; i < 24000; ++i) mono.setSample (0, i, scr.getSample (0, 60000 + i) + scr.getSample (1, 60000 + i));
        const float pitch = analyseStem (mono.getReadPointer (0), 24000, rate).pitchHz;
        p.setSolo (0, false);
        std::printf ("       Screwed (Drop 7 st): %.1f Hz from a 140 Hz voice (want %.1f)\n", pitch, 140.0 * std::exp2 (-7 / 12.0));
        CHECK (std::abs (pitch - 140.0f * std::exp2 (-7.0f / 12.0f)) < 3.0f, "Screwed: hits are pitched down by Drop");
        bool finite = true; for (int i = 0; i < scr.getNumSamples(); ++i) finite &= std::isfinite (scr.getSample (0, i));
        CHECK (finite, "stretched hits stay finite");

        // Stretch: a 0.4 s slice in a 0.5 s step at Gate 100% fills the step.
        set ("patternType", 0.0f);
        auto fill = perform (500, { { 5, 48, true } });
        perform (20, { { 0, 48, false } });
        int gaps = 0;
        for (int i = 40000; i + 1200 < 100000; i += 1200) if (energy (fill, i, 1200) < 0.002f) ++gaps;
        CHECK (gaps <= 2, "a slice shorter than its step is stretched to fill it");
        p.setPlayHead (nullptr);
        (void) jumpRun; (void) latched;
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
