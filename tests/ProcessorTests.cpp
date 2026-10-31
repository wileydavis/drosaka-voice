// Offline checks of the processor (no host needed). Build with -DDV_BUILD_TESTS=ON.
#include "../Source/PluginProcessor.h"
#include <cstdio>
#include <algorithm>

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
        CHECK (r2.getBank()->size() == 2 && r2.getMissingPaths().size() == 1, "moved stem reported missing, rest load");
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
