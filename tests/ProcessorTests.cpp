// Offline checks of the processor (no host needed). Build with -DDV_BUILD_TESTS=ON.
#include "../Source/PluginProcessor.h"
#include "../Source/Tune.h"
#include <cstdio>
#include <algorithm>
#include <set>

static int failures = 0;

// Slice markers as times, from the marker parameters.
static std::vector<double> sliceTimes (const DrosakaVoiceProcessor& p)
{
    const auto sl = p.getSlices();
    return std::vector<double> (sl.t, sl.t + sl.n);
}
static bool near (const std::vector<double>& a, const std::vector<double>& b, double tol = 1e-4)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (std::abs (a[i] - b[i]) > tol) return false;
    return true;
}

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


    std::printf ("harmony (paths)\n");
    {
        auto b = fakeBank ({ 110, 150, 196, 247, 300, 370, 440 }, 3);   // primary (247 Hz) in the middle
        harmony::Voice v[maxVoices];
        // The note (MIDI, fractional) a voice is aimed at.
        auto noteOf = [&] (int s, int rootPc) { return 69.0f + 12.0f * std::log2 (harmony::rootTargetHz (rootPc, b->stems[(size_t) s]->analysis.pitchHz, v[s].semis) / 440.0f); };
        auto selfNote = [&] (int s) { return 69.0f + 12.0f * std::log2 (b->stems[(size_t) s]->analysis.pitchHz / 440.0f); };
        auto pc = [] (float note) { return ((juce::roundToInt (note) % 12) + 12) % 12; };

        harmony::compute (*b, 0.0f, 1, 0.0f, 0, tune::root, nullptr, v);
        bool natural = true; for (int s = 0; s < 7; ++s) natural &= v[s].pull == 0.0f && v[s].strength == 0.0f;
        CHECK (natural, "Spread 0 -> natural speech, nothing pulled");

        harmony::compute (*b, 1.0f / 3.0f, 1, 0.0f, 0, tune::root, nullptr, v);
        bool onRoot = true, close = true;
        for (int s = 0; s < 7; ++s) if (s != 3) { onRoot &= pc (noteOf (s, 0)) == 0 && v[s].pull == 1.0f; close &= std::abs (noteOf (s, 0) - selfNote (s)) <= 6.01f; }
        CHECK (onRoot, "chorus station: every crowd voice sings the root");
        CHECK (close, "each in the octave nearest its own voice (no shift past 6 st)");
        CHECK (v[2].detune * v[4].detune < 0.0f && std::abs (v[0].detune) > std::abs (v[2].detune) && v[2].drift == 1.0f,
               "chorus: detuned to opposite sides, wider further out, drifting");
        CHECK (v[3].pull == 0.0f && v[3].strength == 0.0f, "the primary stays human");

        harmony::compute (*b, 2.0f / 3.0f, 1, 0.0f, 0, tune::root, nullptr, v);   // minor triad on C
        bool triad = true; std::set<int> used;
        for (int s = 0; s < 7; ++s) if (s != 3) { const int c = pc (noteOf (s, 0)); triad &= c == 0 || c == 3 || c == 7; used.insert (c); close &= std::abs (noteOf (s, 0) - selfNote (s)) <= 6.01f; }
        CHECK (triad && used.size() == 3, "chord station: the crowd sings C, Eb and G");
        CHECK (close, "chord tones voiced near each voice's register");

        harmony::compute (*b, 2.0f / 3.0f, 1, 0.0f, 7, tune::root, nullptr, v);   // root G
        triad = true; for (int s = 0; s < 7; ++s) if (s != 3) { const int c = pc (noteOf (s, 7)); triad &= c == 7 || c == 10 || c == 2; }
        CHECK (triad, "the chord follows the Tune root (G minor: G Bb D)");

        bool held[12] {}; held[4] = held[7] = held[11] = true;   // E G B on the harmony keys
        harmony::compute (*b, 0.0f, 1, 0.0f, 0, tune::root, held, v);
        bool keys = true; for (int s = 0; s < 7; ++s) if (s != 3) { const int c = pc (noteOf (s, 0)); keys &= (c == 4 || c == 7 || c == 11) && v[s].pull == 1.0f; }
        CHECK (keys, "held harmony keys override the chord, even with Spread at 0");
        CHECK (pc (noteOf (0, 0)) == 4 && pc (noteOf (6, 0)) == 11, "held notes spread low-to-high across the crowd");

        harmony::compute (*b, 1.0f, 1, 0.0f, 9, tune::minor, nullptr, v);   // A minor cluster
        bool inScale = true; for (int s = 0; s < 7; ++s) if (s != 3) { const int c = pc (noteOf (s, 9)); inScale &= c == 9 || c == 11 || c == 0 || c == 2 || c == 4 || c == 5 || c == 7; }
        CHECK (inScale, "cluster station: neighbouring steps of the Tune scale");

        // Continuity across the whole knob at several Snap settings: target note and pull glide.
        float worst = 0.0f, worstPull = 0.0f;
        for (float snap : { 0.0f, 0.5f, 1.0f })
        {
            harmony::Voice pv[maxVoices];
            harmony::compute (*b, 0.0f, 1, snap, 0, tune::major, nullptr, pv);
            for (int k = 1; k <= 3000; ++k)
            {
                harmony::compute (*b, (float) k / 3000.0f, 1, snap, 0, tune::major, nullptr, v);
                for (int i = 0; i < 7; ++i) { worst = juce::jmax (worst, std::abs (v[i].semis - pv[i].semis)); worstPull = juce::jmax (worstPull, std::abs (v[i].pull - pv[i].pull)); }
                std::copy (v, v + 7, pv);
            }
        }
        std::printf ("       largest step between adjacent knob positions: %.3f st, pull %.3f\n", worst, worstPull);
        CHECK (worst < 0.25f && worstPull < 0.05f, "every voice glides between stations, nothing steps");

        // The pitch map: steady voices land on the note, wobble is ironed out by strength.
        harmony::PitchMap m; m.centre = 200.0f; m.targetHz = 261.63f; m.pull = 1.0f; m.strength = 1.0f;
        CHECK (std::abs (200.0f * harmony::ratio (200.0f, m) - 261.63f) < 0.1f && std::abs (210.0f * harmony::ratio (210.0f, m) - 261.63f) < 0.1f,
               "full strength: every period on the target");
        m.strength = 0.0f;
        CHECK (std::abs (200.0f * harmony::ratio (200.0f, m) - 261.63f) < 0.1f && 210.0f * harmony::ratio (210.0f, m) > 270.0f,
               "zero strength: the median moves, the wobble stays");
        m.pull = 0.0f; m.tune.amount = 0.0f;
        CHECK (harmony::ratio (210.0f, m) == 1.0f, "no pull, no Tune: untouched");
        float d0 = 0, d1 = 0; for (int i = 0; i < 4800; ++i) { const float c = harmony::driftCents (2, i / 100.0, 1.0f); d0 = juce::jmin (d0, c); d1 = juce::jmax (d1, c); }
        CHECK (d1 - d0 > 20.0f && d1 - d0 < 60.0f && harmony::driftCents (2, 3.0, 0.0f) == 0.0f, "Drift wanders a few tens of cents, and Drift 0 is still");
    }

    std::printf ("harmony (audio)\n");
    {
        // Syllabic harmonic tones, so PSOLA has real pitch periods and gaps to keep in time.
        auto syllabic = [&] (float f0, float vib)
        {
            const int len = (int) (rate * 2.0);
            juce::AudioBuffer<float> t (1, len);
            double ph = 0;
            for (int i = 0; i < len; ++i)
            {
                const double tt = i / rate, sp = std::fmod (tt, 0.4);
                const double f = f0 * (1.0 + vib * std::sin (juce::MathConstants<double>::twoPi * 5.0 * tt));
                ph += f / rate;
                double v = 0; for (int h = 1; h <= 12; ++h) v += std::sin (juce::MathConstants<double>::twoPi * h * ph) / h;
                t.setSample (0, i, (float) (0.25 * v * (tt > 0.1 && sp < 0.28 ? std::sin (juce::MathConstants<double>::pi * sp / 0.28) : 0.0)));
            }
            return t;
        };
        auto fPrim = writeWav ("sp_prim.wav", syllabic (150.0f, 0.0f), rate);
        auto fUp = writeWav ("sp_up.wav", syllabic (200.0f, 0.0f), rate);
        DrosakaVoiceProcessor p; p.prepareToPlay (rate, 256);
        juce::StringArray errs; p.addStems ({ fPrim.getFullPathName(), fUp.getFullPathName() }, errs);
        auto set = [&] (const char* id, float v) { auto* prm = p.params.getParameter (id); prm->setValueNotifyingHost (prm->convertTo0to1 (v)); };
        set ("snap", 0.0f); set ("drift", 0.0f);
        p.setSolo (1, true);

        auto render = [&] (float spreadKnob, int chord, int rootPc)
        {
            set ("spread", spreadKnob); set ("chord", (float) chord); set ("tuneRoot", (float) rootPc);
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
        // The 200 Hz voice is the upper neighbour of the primary: chord slot "up 1".
        const float cHz = 261.63f, gHz = 196.0f;
        auto dry = render (0.0f, 0, 0);
        auto chorusC = render (1.0f / 3.0f, 0, 0);   // root C, nearest octave to 200 Hz: C4 (+4.6 st)
        auto fifthG = render (2.0f / 3.0f, 0, 0);    // Fifths on C: up tone = G, nearest 200 Hz: G3
        auto rootG = render (1.0f / 3.0f, 0, 7);     // root G: G3
        const float pd = pitchOf (dry), pc4 = pitchOf (chorusC), pg = pitchOf (fifthG), pr = pitchOf (rootG);
        std::printf ("       natural %.1f Hz, chorus on C %.1f (want ~%.1f), fifth G %.1f (want ~%.1f), root G %.1f\n", pd, pc4, cHz, pg, gHz, pr);
        CHECK (std::abs (pd - 200.0f) < 2.0f, "natural: the voice as recorded");
        CHECK (std::abs (12 * std::log2 (pc4 / cHz)) < 0.3, "chorus: the voice sings the root (within its small detune)");
        CHECK (std::abs (12 * std::log2 (pg / gHz)) < 0.15 && std::abs (12 * std::log2 (pr / gHz)) < 0.3, "chord and root land on G");

        const int lag = envLag (dry, chorusC);
        std::printf ("       timing shift on the root: %d ms\n", lag);
        CHECK (std::abs (lag) <= 5, "pitch shifting keeps the timing (syllables stay put)");
        const double db = 20 * std::log10 (chorusC.getRMSLevel (0, 0, chorusC.getNumSamples()) / dry.getRMSLevel (0, 0, dry.getNumSamples()));
        std::printf ("       level on the root: %+.1f dB\n", db);
        CHECK (std::abs (db) < 2.5, "pitch shifting keeps the level within 2.5 dB");

        // Harmony keys: hold E1 and the voice goes to E (nearest 200 Hz: E3, 164.8 Hz).
        set ("spread", 0.0f); run (p, 256, 500, {});
        juce::MidiBuffer hold; hold.addEvent (juce::MidiMessage::noteOn (1, 40, (juce::uint8) 100), 0);
        {
            juce::AudioBuffer<float> buf (2, 256); buf.clear(); p.processBlock (buf, hold);
        }
        CHECK (p.isHarmonyHeld (4) && ! p.isHarmonyHeld (0), "a harmony key registers as held");
        auto quiet = run (p, 256, 100, {});
        CHECK (quiet.getMagnitude (0, quiet.getNumSamples()) < 1e-4f, "harmony keys don't trigger the phrase");
        auto keyed = run (p, 256, 400, { 0 });
        juce::AudioBuffer<float> km (1, keyed.getNumSamples());
        for (int i = 0; i < keyed.getNumSamples(); ++i) km.setSample (0, i, keyed.getSample (0, i) + keyed.getSample (1, i));
        const float pk = pitchOf (km);
        std::printf ("       holding E1: %.1f Hz (want ~164.8)\n", pk);
        CHECK (std::abs (12 * std::log2 (pk / 164.81f)) < 0.15, "the held key sets the note, at Spread 0");
        CHECK (std::abs (p.getVoiceNote (1) - 52.0f) < 0.2f, "the row readout shows the sung note (E3)");
        juce::MidiBuffer rel; rel.addEvent (juce::MidiMessage::noteOff (1, 40), 0);
        { juce::AudioBuffer<float> buf (2, 256); buf.clear(); p.processBlock (buf, rel); }
        CHECK (! p.isHarmonyHeld (4), "releasing the key lets go");

        // Encoded voices sing the same note.
        set ("encodeCount", 1.0f);
        auto enc = render (1.0f / 3.0f, 0, 0);
        const float pe = pitchOf (enc);
        std::printf ("       encoded, chorus on C: %.1f Hz\n", pe);
        CHECK (std::abs (12 * std::log2 (pe / cHz)) < 0.4, "encoded voices sing the same note as dry ones");
        set ("encodeCount", 0.0f);

        // Wobble: a voice with vibrato is ironed flat at the chord station.
        {
            auto fVib = writeWav ("sp_vib.wav", syllabic (200.0f, 0.03f), rate);
            DrosakaVoiceProcessor q; q.prepareToPlay (rate, 256);
            juce::StringArray e2; q.addStems ({ fPrim.getFullPathName(), fVib.getFullPathName() }, e2);
            auto setq = [&] (const char* id, float v) { auto* prm = q.params.getParameter (id); prm->setValueNotifyingHost (prm->convertTo0to1 (v)); };
            setq ("snap", 0.0f); setq ("drift", 0.0f); q.setSolo (1, true);
            auto wobble = [&] (float knob)
            {
                setq ("spread", knob); run (q, 256, 500, {});
                auto o = run (q, 256, 300, { 0 });
                // Pitch track (autocorrelation) over voiced 25 ms windows: spread in cents.
                std::vector<float> f;
                const int W = 1200;
                for (int at = 4800; at + W + 600 < o.getNumSamples(); at += 480)
                {
                    auto x = [&] (int i) { return o.getSample (0, at + i) + o.getSample (1, at + i); };
                    double e = 0; for (int i = 0; i < W; ++i) e += x (i) * x (i);
                    if (e / W < 0.002) continue;
                    int best = 0; double bv = 0;
                    for (int lag = 120; lag <= 480; ++lag)
                    {
                        double acc = 0, n1 = 0, n2 = 0;
                        for (int i = 0; i < W; ++i) { acc += x (i) * x (i + lag); n2 += x (i + lag) * x (i + lag); }
                        n1 = e; acc /= std::sqrt (n1 * n2 + 1e-12);
                        if (acc > bv) { bv = acc; best = lag; }
                    }
                    if (bv > 0.8 && best > 0)
                    {   // parabolic refinement
                        auto r = [&] (int lag) { double a2 = 0; for (int i = 0; i < W; ++i) a2 += x (i) * x (i + lag); return a2; };
                        const double y0 = r (best - 1), y1 = r (best), y2 = r (best + 1);
                        const double d = 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2);
                        f.push_back (1200.0f * std::log2 ((float) (rate / (best + d)) / 200.0f));
                    }
                }
                if (f.size() < 4) return 999.0f;
                std::sort (f.begin(), f.end());
                return f[f.size() * 9 / 10] - f[f.size() / 10];
            };
            const float wNat = wobble (0.0f), wChord = wobble (2.0f / 3.0f);
            std::printf ("       vibrato spread: natural %.0f cents, chord station %.0f\n", wNat, wChord);
            CHECK (wNat > 40.0f && wChord < wNat * 0.35f, "the chord station irons out the wobble");
        }

        // A slow sweep of the whole knob while the phrase plays: no clicks, no level holes.
        set ("drift", 0.35f); set ("tuneRoot", 0.0f);
        set ("spread", 0.0f);
        run (p, 256, 500, {});
        auto sweep = run (p, 256, 400, { 0 }, [&] (int blk) { set ("spread", juce::jlimit (0.0f, 1.0f, (float) blk / 380.0f)); });
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
        CHECK (near (sliceTimes (p), { 0.5, 1.0, 1.5 }) && p.getSlices().numRegions() == 3, "markers stay sorted; near-duplicates ignored; 3 markers = 3 slices");
        CHECK (p.getSlices().slot[0] == 0 && p.getSlices().slot[1] == 1 && p.getSlices().slot[2] == 2, "markers added out of order still number in time order");
        p.moveSlice (1, 0.2);   // can't cross its left neighbour
        CHECK (std::abs (sliceTimes (p)[1] - 0.512) < 1e-4, "a dragged marker stops at its neighbour");
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

        auto whole = play (30);   // not a slice key: the whole usable phrase, first marker to the end
        auto s2 = play (60);      // C3: the first slice = 0.5 .. 1.0 s
        const int r0 = L, r1 = L + (int) (0.5 * rate);
        CHECK (mag (whole, L + 200, 2000) > 0.01f && mag (whole, L + (int) (1.2 * rate), 2000) > 0.01f && mag (whole, L + (int) (1.55 * rate), 4000) < 1e-4f,
               "other keys play the usable phrase: from the first marker to the end");
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

        auto last = play (62);    // D3: the last slice = 1.5 s .. end
        CHECK (mag (last, L + 200, (int) (0.4 * rate)) > 0.01f && mag (last, L + (int) (0.6 * rate), 4000) < 1e-4f, "last slice runs to the end of the phrase");

        p.removeSlice (0); p.removeSlice (0); p.removeSlice (0);
        CHECK (sliceTimes (p).empty() && p.getSlices().numRegions() == 0, "removing every marker: back to whole-phrase playback");
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
        CHECK (near (sliceTimes (q), { 0.7, 1.3 }), "slice markers come back");
        CHECK (std::abs (q.params.getParameter ("spread")->getValue() - 0.4f) < 1e-4f && (int) q.params.getRawParameterValue ("spatialMode")->load() == 2,
               "knob states come back");
        CHECK (std::abs (qb->stems[0]->analysis.pitchHz - 120.0f) < 3.0f && ! qb->stems[0]->marks.pos.empty(), "analysis rebuilt from the embedded audio");

        auto presetFile = tmp.getChildFile ("bank.dvoice");
        CHECK (q.savePreset (presetFile) && presetFile.getSize() > 1000, "Save preset writes a file");
        DrosakaVoiceProcessor r; r.prepareToPlay (rate, 256);
        CHECK (r.loadPreset (presetFile) && r.getBank()->size() == 3 && sliceTimes (r).size() == 2, "Load preset brings the whole bank back");
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
        // 16 steps of 1/16 per bar, from beat 0.
        struct Run { pattern::State st; double ppq = 0.0; std::vector<std::vector<pattern::Hit>> v; };
        auto go = [] (Run& r, const pattern::Params& pp, int regions, int steps)
        {
            for (int k = 0; k < steps; ++k)
            {
                pattern::Hit h[8]; int nh = 0;
                const double len = pattern::nextStep (r.st, pp, regions, r.ppq, h, nh);
                r.v.emplace_back (h, h + nh);
                r.ppq += len;
            }
        };
        auto slicesOf = [] (const std::vector<std::vector<pattern::Hit>>& v, size_t from = 0, size_t to = 1u << 30)
        {
            std::vector<int> r; for (size_t i = from; i < juce::jmin (to, v.size()); ++i) r.push_back (v[i].empty() ? -1 : v[i][0].slice); return r;
        };
        using V = std::vector<int>;
        pattern::Params base; base.division = 0.25;

        { Run r; auto pp = base; go (r, pp, 6, 20);
          CHECK (slicesOf (r.v, 0, 8) == V ({ 0, 1, 2, 3, 4, 5, 0, 1 }), "Phrase: every slice in order, looping");
          CHECK (r.v[15][0].slice == 3 && r.v[16][0].slice == 0, "Phrase: the loop restarts on the bar line"); }

        { Run r; r.st.start = 2; go (r, base, 6, 6);
          CHECK (slicesOf (r.v) == V ({ 2, 3, 4, 5, 0, 1 }), "the trigger slice starts the loop, which runs on through every slice"); }

        { Run r; auto pp = base; pp.length = 0.0f; go (r, pp, 6, 4);
          CHECK (slicesOf (r.v) == V ({ 0, 0, 0, 0 }), "Length 0: the trigger slice on repeat");
          Run r2; pp.length = 0.4f; go (r2, pp, 6, 7);
          CHECK (slicesOf (r2.v) == V ({ 0, 1, 2, 0, 1, 2, 0 }), "Length 40% of 6 slices: a 3-slice loop"); }

        { Run r; auto pp = base; go (r, pp, 6, 8);
          pp.length = 0.0f; go (r, pp, 6, 10);
          CHECK (slicesOf (r.v, 8, 16) == V ({ 2, 3, 4, 5, 0, 1, 2, 3 }) && slicesOf (r.v, 16, 18) == V ({ 0, 0 }),
                 "a Length change waits for the loop's restart (the next bar line)"); }

        { Run big; auto pl = base; pl.fill = 0.25f; go (big, pl, 8, 32);   // 4 hits a bar, 8 slices: a 2-bar loop
          CHECK (slicesOf (big.v, 16, 17) == V ({ 4 }) && slicesOf (big.v, 32 - 16, 32 - 15) == V ({ 4 }), "a long phrase spans as many bars as it needs");
          Run b2; go (b2, pl, 8, 48);
          CHECK (b2.v[32][0].slice == 0, "and restarts on the bar after it ends"); }

        { Run r; auto pp = base; pp.fill = 5.0f / 16.0f; go (r, pp, 8, 16);
          std::vector<int> on; for (int i = 0; i < 16; ++i) if (! r.v[(size_t) i].empty()) on.push_back (i);
          CHECK (on == V ({ 0, 4, 7, 10, 13 }), "Fill 5 of 16: hits spread evenly (Euclidean), first on the downbeat");
          CHECK (slicesOf (r.v, 0, 16) == V ({ 0, -1, -1, -1, 1, -1, -1, 2, -1, -1, 3, -1, -1, 4, -1, -1 }), "the phrase picks up where it left off on the next hit"); }

        { Run r; auto pp = base; pp.scheme = pattern::anchor; go (r, pp, 6, 16);
          CHECK (slicesOf (r.v) == V ({ 0, 1, 2, 3, 0, 4, 5, 1, 0, 2, 3, 4, 0, 5, 1, 2 }), "Anchor: the trigger slice on every beat, the phrase between"); }

        { Run r; auto pp = base; pp.scheme = pattern::offbeat; go (r, pp, 6, 16);
          std::vector<int> on; for (int i = 0; i < 16; ++i) if (! r.v[(size_t) i].empty()) on.push_back (i);
          CHECK (on == V ({ 2, 6, 10, 14 }) && r.v[2][0].gate < 0.5f && r.v[2][0].length == 0.5 && slicesOf (r.v, 6, 7) == V ({ 1 }), "Offbeat: stabs on the \"and\"s (half an eighth long), in phrase order"); }

        { Run r; auto pp = base; pp.scheme = pattern::rotate; go (r, pp, 4, 48);
          CHECK (r.v[0][0].slice == 0 && r.v[16][0].slice == 1 && r.v[32][0].slice == 2, "Rotate: each bar starts one slice later"); }

        { Run r; auto pp = base; pp.scheme = pattern::polymeter; go (r, pp, 3, 20);
          CHECK (slicesOf (r.v, 14, 20) == V ({ 2, 0, 1, 2, 0, 1 }), "Polymeter: 3 slices cycle straight through the bar line"); }

        { Run r; auto pp = base; pp.scheme = pattern::pingPong; go (r, pp, 4, 9);
          CHECK (slicesOf (r.v) == V ({ 0, 1, 2, 3, 2, 1, 0, 1, 2 }), "Ping-pong: forward, then back"); }

        { Run r; auto pp = base; pp.scheme = pattern::callAnswer; go (r, pp, 4, 32);
          bool answer = true; for (int i = 16; i < 32; ++i) answer &= r.v[(size_t) i][0].slice == 0 && r.v[(size_t) i][0].gate <= 0.5f;
          CHECK (slicesOf (r.v, 0, 5) == V ({ 0, 1, 2, 3, 0 }) && answer && r.v[17].size() >= 2, "Call & answer: the phrase, then a bar of the trigger slice, chopped"); }

        { Run r; auto pp = base; pp.scheme = pattern::shuffle; go (r, pp, 5, 16 * 9);
          auto first = slicesOf (r.v, 0, 5); auto sorted = first; std::sort (sorted.begin(), sorted.end());
          CHECK (sorted == V ({ 0, 1, 2, 3, 4 }), "Shuffle: every slice once per pass");
          CHECK (slicesOf (r.v, 16, 32) != slicesOf (r.v, 0, 16) && slicesOf (r.v, 128, 144) == slicesOf (r.v, 0, 16), "a new order each bar, the same 8 bars every time"); }

        { Run r; auto pp = base; pp.scheme = pattern::screwed; pp.drop = 7.0f; go (r, pp, 3, 2);
          CHECK (r.v[0][0].pitch == -7.0f && r.v[0][0].fit && r.v[0][0].length == 0.5 && r.v[1][0].slice == 1, "Screwed: two steps per slice, pitched down by Drop, fitted"); }

        { Run r; auto pp = base; pp.scheme = pattern::build; pp.division = 0.5; go (r, pp, 4, 30);
          CHECK (r.v[0][0].length == 0.5 && r.v[7][0].length == 0.5 && r.v[8][0].length == 0.25 && r.v[23][0].length == 0.25 && r.v[24][0].length == 0.125,
                 "Build: the division halves every bar"); }

        { Run r; auto pp = base; pp.stutter = 1.0f; go (r, pp, 4, 16 * 8);
          int rollsIn[8] {}; bool sane = true;
          for (int i = 0; i < 16 * 8; ++i)
          {
              const auto& st = r.v[(size_t) i];
              if (st.size() < 2) continue;
              ++rollsIn[i / 16];
              double sum = 0; for (auto& h : st) { sum += h.length; sane &= h.slice == st[0].slice; }
              sane &= std::abs (sum - 0.25) < 1e-9 && st.size() <= 6;
          }
          std::printf ("       Stutter 100%%, rolls per bar: %d %d %d %d %d %d %d %d\n", rollsIn[0], rollsIn[1], rollsIn[2], rollsIn[3], rollsIn[4], rollsIn[5], rollsIn[6], rollsIn[7]);
          CHECK (sane, "Stutter: a roll is 2-6 retriggers of the same slice, filling its step");
          CHECK (rollsIn[3] + rollsIn[7] > rollsIn[0] + rollsIn[1], "rolls gather toward the end of each 4 bars");
          Run z; go (z, base, 4, 64); bool none = true; for (auto& st : z.v) none &= st.size() == 1; CHECK (none, "Stutter 0: no rolls"); }

        { Run r; go (r, base, 4, 64);
          CHECK (slicesOf (r.v, 0, 16) == slicesOf (r.v, 48, 64), "Variation 0: every bar the same");
          Run w; auto pp = base; pp.variation = 1.0f; go (w, pp, 4, 16 * 16);
          int changed[8] {}; int reversed = 0, ghosts = 0;
          for (int i = 0; i < 16 * 8; ++i)
          {
              const auto& st = w.v[(size_t) i];
              if (st.empty() || st[0].slice != i % 4 || std::abs (st[0].pitch) > 0 || st[0].reverse) ++changed[i / 16];
              for (auto& h : st) { reversed += h.reverse; ghosts += h.gain < 0.5f; }
          }
          std::printf ("       Variation 100%%, changed steps per bar: %d %d %d %d %d %d %d %d (reversed %d)\n",
                       changed[0], changed[1], changed[2], changed[3], changed[4], changed[5], changed[6], changed[7], reversed);
          CHECK (changed[0] > 0 && changed[7] > changed[0] && reversed > 0, "Variation: bars stray from the loop, bar 8 the most, some slices backwards");
          bool same = true;
          for (int i = 0; i < 16 * 8; ++i) same &= slicesOf (w.v, (size_t) i, (size_t) i + 1) == slicesOf (w.v, (size_t) i + 128, (size_t) i + 129);
          CHECK (same, "and the same 8 bars every time"); }

        { Run r; auto pp = base; pp.swing = 1.0f; go (r, pp, 4, 4);
          CHECK (r.v[0][0].offset == 0.0 && std::abs (r.v[1][0].offset - 0.125) < 1e-9 && std::abs (r.v[1][0].length - 0.125) < 1e-9,
                 "Swing 75%: off-steps half a step late, still ending on time");
          Run t; pp.division = 1.0 / 6.0; go (t, pp, 4, 4);
          CHECK (t.v[1][0].offset == 0.0, "no swing on triplet grids"); }

        { Run r; auto pp = base; pp.accent = 1.0f; go (r, pp, 4, 4);
          CHECK (r.v[0][0].gain == 1.0f && std::abs (r.v[2][0].gain - 0.7f) < 1e-6f && std::abs (r.v[1][0].gain - 0.45f) < 1e-6f,
                 "Accent: beats full, \"and\"s softer, 16ths softest"); }

        { Run r; go (r, base, 6, 1);
          r.st.jump = 4; go (r, base, 6, 2);
          CHECK (slicesOf (r.v) == V ({ 0, 4, 5 }), "a held slice key: the loop restarts there on the next step"); }

        CHECK (pattern::schemeFromOldType (5) == pattern::build && pattern::schemeFromOldType (6) == pattern::shuffle && pattern::schemeFromOldType (3) == pattern::screwed,
               "old pattern types map to the nearest scheme");
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
        set ("playMode", 1.0f); set ("scheme", 0.0f); set ("patternDiv", 1.0f); set ("gate", 0.5f);   // Phrase, 1/8, half-length hits
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
        std::printf ("       Phrase 1/8: %d of %d hits start exactly on their grid line\n", onTime, total);
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
        set ("scheme", (float) pattern::screwed); set ("dropSemis", 7.0f); set ("gate", 1.0f); set ("patternDiv", 0.0f);   // 1/4 steps: 0.5 s
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

        // Everything on: rolls, variation (reversed slices), swing, accent. Stays sane.
        set ("scheme", (float) pattern::anchor); set ("patternDiv", 2.0f); set ("stutter", 1.0f); set ("variation", 1.0f);
        set ("swing", 0.6f); set ("accent", 0.8f); set ("dropSemis", 5.0f);
        auto wild = perform (1500, { { 5, 48, true } });
        perform (20, { { 0, 48, false } });
        bool wildFinite = true; float wildPeak = 0.0f;
        for (int i = 0; i < wild.getNumSamples(); ++i) { wildFinite &= std::isfinite (wild.getSample (0, i)); wildPeak = juce::jmax (wildPeak, std::abs (wild.getSample (0, i))); }
        std::printf ("       all macros up: peak %.2f\n", wildPeak);
        CHECK (wildFinite && wildPeak > 0.05f && wildPeak < 1.5f, "all macros up (rolls, reversed slices, swing, accent): sound, finite, no blow-ups");
        set ("stutter", 0.0f); set ("variation", 0.0f); set ("swing", 0.0f); set ("accent", 0.0f); set ("patternDiv", 0.0f);

        // Stretch: a 0.4 s slice in a 0.5 s step at Gate 100% fills the step.
        set ("scheme", 0.0f);
        auto fill = perform (500, { { 5, 48, true } });
        perform (20, { { 0, 48, false } });
        int gaps = 0;
        for (int i = 40000; i + 1200 < 100000; i += 1200) if (energy (fill, i, 1200) < 0.002f) ++gaps;
        CHECK (gaps <= 2, "a slice shorter than its step is stretched to fill it");
        // A phrase that opens with a pause: hits start on its first sound, so stabs speak.
        {
            auto t = voiceTone (150.0f, 0.4f, rate, (int) (rate * 1.5));
            for (int i = 0; i < (int) (rate * 0.3); ++i) t.setSample (0, i, 0.0f);
            auto fq = writeWav ("pt_pause.wav", t, rate);
            DrosakaVoiceProcessor q; q.prepareToPlay (rate, 256);
            TestPlayHead qh; qh.bpm = 120.0; qh.ppq = 0.0; q.setPlayHead (&qh);
            juce::StringArray e2; q.addStems ({ fq.getFullPathName() }, e2);
            auto setq = [&] (const char* id, float v) { auto* prm = q.params.getParameter (id); prm->setValueNotifyingHost (prm->convertTo0to1 (v)); };
            setq ("playMode", 1.0f); setq ("scheme", (float) pattern::offbeat); setq ("patternDiv", 2.0f); setq ("latch", 1.0f);
            juce::AudioBuffer<float> o (2, 256 * 400), blk (2, 256);
            for (int k = 0; k < 400; ++k)
            {
                juce::MidiBuffer m; if (k == 2) m.addEvent (juce::MidiMessage::noteOn (1, 48, (juce::uint8) 100), 0);
                q.processBlock (blk, m); qh.ppq += 256.0 / spb;
                for (int ch = 0; ch < 2; ++ch) o.copyFrom (ch, k * 256, blk, ch, 0, 256);
            }
            const int stab = (int) std::round (2.5 * spb) + q.getLatencySamples();   // the "and" of beat 3
            const float e = energy (o, stab + 200, 1800);
            std::printf ("       offbeat stab from a phrase with a 300 ms pause: level %.3f\n", e);
            CHECK (e > 0.05f, "pattern hits skip a slice's opening pause (stabs still speak)");
            q.setPlayHead (nullptr);
        }

        // A set saved before the rework: its "patternType" becomes the nearest scheme.
        {
            juce::MemoryBlock mb; p.getStateInformation (mb);
            auto tree = juce::ValueTree::readFromData (static_cast<const char*> (mb.getData()) + 4, mb.getSize() - 4);
            auto pt = tree.getChildWithName (p.params.state.getType());
            pt.removeChild (pt.getChildWithProperty ("id", "scheme"), nullptr);
            juce::ValueTree old ("PARAM"); old.setProperty ("id", "patternType", nullptr); old.setProperty ("value", 5, nullptr);   // Ratchet
            pt.appendChild (old, nullptr);
            juce::MemoryOutputStream mos; mos.write (mb.getData(), 4); tree.writeToStream (mos);
            DrosakaVoiceProcessor q; q.setStateInformation (mos.getData(), (int) mos.getDataSize());
            CHECK ((int) q.params.getRawParameterValue ("scheme")->load() == pattern::build, "old sets: Ratchet comes back as Build");
        }
        p.setPlayHead (nullptr);
        (void) jumpRun; (void) latched;
    }


    std::printf ("drop, human primary, depth\n");
    {
        juce::StringArray paths;
        const float f0s[] = { 110, 130, 150, 175, 200 };
        for (int i = 0; i < 5; ++i)
            paths.add (writeWav ("dr" + juce::String (i) + ".wav", voiceTone (f0s[i], 0.3f, rate, (int) rate * 3), rate).getFullPathName());
        DrosakaVoiceProcessor p; p.prepareToPlay (rate, 256);
        juce::StringArray errs; p.addStems (paths, errs);
        p.setPrimary (2);
        auto set = [&] (const char* id, float v) { auto* prm = p.params.getParameter (id); prm->setValueNotifyingHost (prm->convertTo0to1 (v)); };
        set ("count", 1.0f); set ("encodeCount", 1.0f); set ("spread", 2.0f / 3.0f);

        // Keep primary human: Encode 100% takes every voice except the primary.
        set ("keepHuman", 1.0f);
        run (p, 256, 40, { 0 });
        int enc = 0; for (int s2 = 0; s2 < 5; ++s2) enc += p.getVoiceEncode (s2) > 0.5f ? 1 : 0;
        CHECK (enc == 4 && p.getVoiceEncode (2) < 0.01f && p.audibleVoices.load() == 4, "Keep primary human: Encode 100% stops short of the primary");
        set ("keepHuman", 0.0f);
        run (p, 256, 40, {});
        CHECK (p.getVoiceEncode (2) > 0.99f, "without it, the primary falls last as before");

        // Drop: mid-phrase, the crowd fades out and the primary comes back human, dead centre.
        run (p, 256, 700, {});
        auto natural = run (p, 256, 300, { 0 });
        run (p, 256, 700, {});
        auto dropped = run (p, 256, 300, { 0 }, [&] (int blk) { if (blk == 100) set ("drop", 1.0f); });
        bool onlyPrimary = true;
        for (int s2 = 0; s2 < 5; ++s2) onlyPrimary &= s2 == 2 ? p.getVoiceLevel (s2) > 0.99f : p.getVoiceLevel (s2) < 0.01f;
        CHECK (onlyPrimary && p.getVoiceEncode (2) < 0.01f, "Drop: only the primary, and it's human");
        float lr = 0; for (int i = 256 * 200; i < 256 * 300; ++i) lr = juce::jmax (lr, std::abs (dropped.getSample (0, i) - dropped.getSample (1, i)));
        CHECK (lr < 1e-4f, "Drop: the primary sits dead centre");
        CHECK (maxJump (dropped) < maxJump (natural) * 1.3f, "Drop cuts in without a click");
        set ("drop", 0.0f);
        run (p, 256, 60, {});
        int back = 0; for (int s2 = 0; s2 < 5; ++s2) back += p.getVoiceLevel (s2) > 0.01f ? 1 : 0;
        CHECK (back == 5, "releasing Drop brings the crowd back");

        // Depth: the outermost voice gets quieter, later and wetter; the primary is untouched.
        set ("encodeCount", 0.0f); set ("spread", 0.0f);
        auto soloRun = [&] (int voice, float depth)
        {
            set ("depth", depth);
            p.setSolo (voice, true);
            run (p, 256, 800, {});
            auto o = run (p, 256, 900, { 0 });
            p.setSolo (voice, false);
            return o;
        };
        auto onset = [] (const juce::AudioBuffer<float>& o) { for (int i = 0; i < o.getNumSamples(); ++i) if (std::abs (o.getSample (0, i)) + std::abs (o.getSample (1, i)) > 0.01f) return i; return -1; };
        auto rms = [] (const juce::AudioBuffer<float>& o, int a, int n2) { return o.getRMSLevel (0, a, n2) + o.getRMSLevel (1, a, n2); };
        // In the solo crowd layout the pair index is fixed by the bank, so voice 0 (lowest) is outermost.
        auto outer0 = soloRun (0, 0.0f), outer1 = soloRun (0, 1.0f);
        auto prim0 = soloRun (2, 0.0f), prim1 = soloRun (2, 1.0f);
        const int delayMs = juce::roundToInt ((onset (outer1) - onset (outer0)) / 48.0);
        const double levelDb = 20 * std::log10 (rms (outer1, 40000, 48000) / rms (outer0, 40000, 48000));
        const int end = onset (outer0) + (int) (rate * 3) + 2000;
        std::printf ("       outer voice at Depth 100%%: +%d ms later, %+.1f dB, tail after the phrase %.4f\n", delayMs, levelDb, rms (outer1, end, 9600));
        CHECK (delayMs >= 10 && delayMs <= 13, "Depth: the outer voice arrives ~12 ms later");
        CHECK (levelDb < -1.0 && levelDb > -6.0, "Depth: and a few dB quieter (with its reverb)");
        CHECK (rms (outer1, end, 9600) > 0.002f && rms (outer0, end, 9600) < 1e-4f, "Depth: and leaves a reverb tail");
        float diff = 0; for (int i = 0; i < prim0.getNumSamples(); ++i) diff = juce::jmax (diff, std::abs (prim0.getSample (0, i) - prim1.getSample (0, i)));
        CHECK (diff < 1e-5f, "Depth never touches the primary");
        set ("depth", 0.0f);
    }


    std::printf ("MIDI mapping\n");
    {
        DrosakaVoiceProcessor p; p.prepareToPlay (rate, 256);
        auto& ps = p.getParameters();
        bool allAuto = true; juce::StringArray ids;
        for (auto* prm : ps)
        {
            allAuto &= prm->isAutomatable();
            if (auto* w = dynamic_cast<juce::AudioProcessorParameterWithID*> (prm)) ids.add (w->paramID);
        }
        std::printf ("       %d parameters: %s\n", ps.size(), ids.joinIntoString (" ").toRawUTF8());
        CHECK (allAuto && ps.size() <= 64, "every control is an automatable host parameter (and few enough for Live to list them)");
        // VST3 hosts also get an 8-character short name; Live tells parameters apart by it.
        juce::StringArray shortNames; bool unique = true;
        for (auto* prm : ps)
        {
            const auto sn = prm->getName (8).trim().toLowerCase();
            if (shortNames.contains (sn)) { unique = false; std::printf ("       duplicate short name: '%s'\n", sn.toRawUTF8()); }
            shortNames.add (sn);
        }
        CHECK (unique, "no two parameters share a short (8-character) name");

        auto cc = [&] (int number, int value)
        {
            juce::AudioBuffer<float> blk (2, 256); juce::MidiBuffer m;
            m.addEvent (juce::MidiMessage::controllerEvent (3, number, value), 10);
            p.processBlock (blk, m);
            p.applyPendingMidi();   // the message-thread step (a 60 Hz timer in the plugin)
        };
        p.armMidiLearn ("spread");
        CHECK (p.getLearningParam() == "spread", "arming learn on a knob");
        cc (21, 64);
        CHECK (p.getCcFor ("spread") == 21 && p.getLearningParam().isEmpty(), "the next CC (any channel) is bound to it");
        cc (21, 127);
        CHECK (std::abs (p.params.getParameter ("spread")->getValue() - 1.0f) < 1e-6f, "the CC drives the knob, through the host");
        cc (21, 0);
        CHECK (p.params.getParameter ("spread")->getValue() < 1e-6f, "both ways");

        p.armMidiLearn ("scheme");   // a stepped knob
        cc (22, 0);
        cc (22, 127);
        CHECK ((int) p.params.getRawParameterValue ("scheme")->load() == pattern::numSchemes - 1, "stepped knobs step through their choices");

        p.armMidiLearn ("spread");
        cc (30, 10);
        CHECK (p.getCcFor ("spread") == 30, "re-learning moves the knob to the new CC");
        cc (21, 127);
        CHECK (p.params.getParameter ("spread")->getValue() < 0.1f, "and the old CC lets go of it");

        juce::MemoryBlock mb; p.getStateInformation (mb);
        DrosakaVoiceProcessor q; q.setStateInformation (mb.getData(), (int) mb.getSize());
        CHECK (q.getCcFor ("spread") == 30 && q.getCcFor ("scheme") == 22, "mappings save with the set");
        p.forgetCc ("spread");
        CHECK (p.getCcFor ("spread") == -1, "Forget removes a mapping");
    }


    std::printf ("slice markers: homes + automatable offsets\n");
    {
        auto tA = voiceTone (140.0f, 0.4f, rate, (int) (rate * 2.0));
        auto fA = writeWav ("sm_a.wav", tA, rate);
        DrosakaVoiceProcessor p; p.prepareToPlay (rate, 256);
        juce::StringArray errs; p.addStems ({ fA.getFullPathName() }, errs);
        const int L = p.getLatencySamples();
        auto* o1 = p.params.getParameter ("sliceOffset1");
        auto* o2 = p.params.getParameter ("sliceOffset2");
        CHECK (o1 != nullptr && o1->isAutomatable() && p.params.getParameter ("sliceOffset15") != nullptr, "15 automatable marker-offset parameters");
        CHECK (o1->getName (64) == "C3 slice offset" && p.params.getParameter ("sliceOffset15")->getName (64) == "D4 slice offset",
               "named for the key whose slice each marker starts");
        CHECK (std::abs (o1->getDefaultValue() - 0.5f) < 1e-6f && o1->getCurrentValueAsText() == "0%", "an offset lane starts at zero, in the middle");

        p.addSlice (0.5); p.addSlice (1.5);
        auto ms = [&] (juce::AudioProcessorParameter* prm, float v) { prm->setValueNotifyingHost (dynamic_cast<juce::RangedAudioParameter*> (prm)->convertTo0to1 (v)); };
        // Homes at 0.5 and 1.5 s in a 2 s phrase.
        ms (o1, 50.0f);
        CHECK (near (sliceTimes (p), { 0.995, 1.5 }), "+50% moves C3's marker half way to C#3's");
        ms (o1, 100.0f);
        CHECK (near (sliceTimes (p), { 1.49, 1.5 }), "+100% takes it right up to the next marker");
        ms (o1, -100.0f);
        CHECK (near (sliceTimes (p), { 0.01, 1.5 }), "-100% takes it back to the start");
        ms (o1, -50.0f);
        CHECK (near (sliceTimes (p), { 0.255, 1.5 }), "-50% half way back");
        ms (o1, 0.0f); ms (o2, 100.0f);
        CHECK (near (sliceTimes (p), { 0.5, 1.99 }), "the last marker's +100% is the end of the audio");
        ms (o2, 0.0f);
        CHECK (near (sliceTimes (p), { 0.5, 1.5 }), "back to the middle: back home");
        ms (o2, -100.0f); ms (o1, 100.0f);
        CHECK (near (sliceTimes (p), { 1.49, 1.498 }), "markers moving toward each other never overtake");
        ms (o1, 0.0f); ms (o2, 0.0f);

        // Playback follows the offset.
        ms (o1, 25.0f);    // C3 slice now 0.7475 .. 1.5 s (0.7525 s long)
        run (p, 256, 500, {});
        juce::AudioBuffer<float> out (2, 256 * 450), blk (2, 256);
        for (int k = 0; k < 450; ++k)
        {
            juce::MidiBuffer m; if (k == 0) m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            p.processBlock (blk, m);
            for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, k * 256, blk, ch, 0, 256);
        }
        CHECK (out.getMagnitude (0, L + (int) (0.7 * rate), 2000) > 0.01f && out.getMagnitude (0, L + (int) (0.7525 * rate) + 20, 4000) < 1e-4f,   // slice length 1.5 - 0.7475 s
               "the slice key plays the offset span");
        ms (o1, 0.0f);

        // Dragging on the waveform moves the home (the marker lands where you drop it, offset kept).
        ms (o1, 20.0f);
        p.moveSlice (0, 0.9);
        CHECK (near (sliceTimes (p), { 0.9, 1.5 }) && std::abs (p.getSlices().base[0] - 0.7525) < 1e-3, "dragging an offset marker keeps its offset, moves its home");
        ms (o1, 0.0f);
        p.moveSlice (0, 0.5);

        // Adding a marker before others renumbers them; their offsets travel with them.
        ms (o1, 20.0f);
        p.addSlice (0.2);
        CHECK (sliceTimes (p).size() == 3 && std::abs (sliceTimes (p)[0] - 0.2) < 1e-4 && std::abs (o2->getValue() - 0.6f) < 1e-3f,
               "inserting a marker renumbers the later ones and carries their offsets");
        p.removeSlice (0);
        CHECK (near (sliceTimes (p), { 0.698, 1.5 }) && std::abs (o1->getValue() - 0.6f) < 1e-3f, "deleting one does too");

        // Saved with the set.
        juce::MemoryBlock mb; p.getStateInformation (mb);
        DrosakaVoiceProcessor q; q.setStateInformation (mb.getData(), (int) mb.getSize());
        CHECK (near (sliceTimes (q), { 0.698, 1.5 }), "homes and offsets reopen with the set");

        // Markers from the previous build: absolute positions, possibly out of slot order.
        juce::ValueTree st ("DrosakaVoice");
        st.setProperty ("version", 4, nullptr);
        juce::ValueTree pt ("PARAMS");
        auto addParam = [&] (const char* id, double v) { juce::ValueTree c ("PARAM"); c.setProperty ("id", id, nullptr); c.setProperty ("value", v, nullptr); pt.appendChild (c, nullptr); };
        addParam ("slice1", 0.75); addParam ("slice2", 0.25); addParam ("slice3", 0.0);   // 1.5 s and 0.5 s, out of order
        st.appendChild (pt, nullptr);
        juce::ValueTree bt ("Bank");
        juce::ValueTree v ("Stem"); v.setProperty ("path", fA.getFullPathName(), nullptr); bt.appendChild (v, nullptr);
        st.appendChild (bt, nullptr);
        juce::MemoryBlock omb; juce::AudioProcessor::copyXmlToBinary (*st.createXml(), omb);
        DrosakaVoiceProcessor o; o.setStateInformation (omb.getData(), (int) omb.getSize());
        CHECK (near (sliceTimes (o), { 0.5, 1.5 }), "existing markers from the last build are picked up, in time order");
        ms (o.params.getParameter ("sliceOffset1"), 10.0f);
        CHECK (near (sliceTimes (o), { 0.599, 1.5 }), "and 'C#3 slice offset' moves the existing first marker");

        // The older-still format (seconds in the bank) too.
        juce::ValueTree old ("DrosakaVoice");
        old.setProperty ("version", 3, nullptr);
        juce::ValueTree bt2 ("Bank");
        bt2.setProperty ("slices", "1.2,0.4", nullptr);
        juce::ValueTree v2 ("Stem"); v2.setProperty ("path", fA.getFullPathName(), nullptr); bt2.appendChild (v2, nullptr);
        old.appendChild (bt2, nullptr);
        juce::MemoryBlock omb2; juce::AudioProcessor::copyXmlToBinary (*old.createXml(), omb2);
        DrosakaVoiceProcessor older; older.setStateInformation (omb2.getData(), (int) omb2.getSize());
        CHECK (near (sliceTimes (older), { 0.4, 1.2 }), "older sets' markers are picked up too");
    }

    std::printf ("autotune\n");
    {
        // Targets.
        CHECK (std::abs (tune::target (155.0f, 0, tune::root, 140.0f) - 130.81f) < 0.1f, "Root C: a voice centred at 140 Hz sits on C3 (130.8)");
        CHECK (std::abs (tune::target (155.0f, 0, tune::root, 300.0f) - 261.63f) < 0.1f, "Root keeps each voice in its own octave");
        CHECK (std::abs (tune::target (450.0f, 9, tune::minor, 0) - 440.0f) < 0.1f && std::abs (tune::target (480.0f, 9, tune::minor, 0) - 493.88f) < 0.2f,
               "Minor on A snaps to A and B");
        CHECK (std::abs (tune::target (300.0f, 0, tune::major, 0) - 293.66f) < 0.2f && std::abs (tune::target (250.0f, 0, tune::major, 0) - 246.94f) < 0.2f,
               "Major on C snaps to D and B");
        CHECK (tune::ratio (150.0f, 0.0f, 0, tune::root, 150.0f) == 1.0f, "Tune 0 = untouched");

        // A wobbly, gliding voice: 150 Hz with +-1 st vibrato at 5 Hz plus a slow 2 st rise.
        const int len = (int) (rate * 3.0);
        juce::AudioBuffer<float> wob (1, len); wob.clear();
        double ph = 0.0;
        for (int i = 0; i < len; ++i)
        {
            const double t = i / rate;
            const double f = 150.0 * std::exp2 ((std::sin (juce::MathConstants<double>::twoPi * 5.0 * t) + 2.0 * t / 3.0) / 12.0);
            ph += f / rate;
            float x = 0.0f;
            for (int h = 1; h < 25; ++h) x += 0.08f / (float) h * (float) std::sin (juce::MathConstants<double>::twoPi * h * ph);
            wob.setSample (0, i, x);
        }
        auto fW = writeWav ("wobble.wav", wob, rate);
        DrosakaVoiceProcessor p; p.prepareToPlay (rate, 256);
        juce::StringArray errs; p.addStems ({ fW.getFullPathName() }, errs);
        auto set = [&] (const char* id, float v) { auto* prm = p.params.getParameter (id); prm->setValueNotifyingHost (prm->convertTo0to1 (v)); };

        auto trackCents = [&] (float targetHz, float& meanAbs, float& spread)
        {
            run (p, 256, 800, {});
            auto out = run (p, 256, 560, { 0 });
            std::vector<float> cents;
            for (int a0 = 12000; a0 + 7200 < 256 * 560 - 4000; a0 += 4800)
            {
                std::vector<float> m (7200);
                for (int i = 0; i < 7200; ++i) m[(size_t) i] = out.getSample (0, a0 + i);
                const float hz = analyseStem (m.data(), 7200, rate).pitchHz;
                if (hz > 0) cents.push_back (1200.0f * std::log2 (hz / targetHz));
            }
            float mean = 0, lo = 1e9, hi = -1e9;
            for (auto c : cents) { mean += std::abs (c); lo = juce::jmin (lo, c); hi = juce::jmax (hi, c); }
            meanAbs = cents.empty() ? 1e9f : mean / (float) cents.size();
            spread = hi - lo;
        };
        float m0, s0, m1, s1, mh, sh;
        trackCents (130.81f, m0, s0);
        set ("tuneAmount", 1.0f); set ("tuneRoot", 0.0f); set ("tuneScale", 0.0f);
        trackCents (130.81f, m1, s1);
        set ("tuneAmount", 0.5f);
        trackCents (130.81f, mh, sh);
        std::printf ("       pitch spread across the phrase: untuned %.0f cents, Tune 50%% %.0f, Tune 100%% %.0f (avg %.0f cents off C3)\n", s0, sh, s1, m1);
        CHECK (s1 < 40.0f && m1 < 25.0f, "Tune 100% Root: the wobbling voice holds on the root");
        CHECK (sh < s0 * 0.75f && sh > s1, "Tune 50% pulls part of the way");

        // Minor: notes land on scale degrees.
        set ("tuneAmount", 1.0f); set ("tuneScale", 1.0f); set ("tuneRoot", 9.0f);   // A minor
        run (p, 256, 800, {});
        auto out = run (p, 256, 560, { 0 });
        int onScale = 0, total = 0;
        for (int a0 = 12000; a0 + 7200 < 256 * 560 - 4000; a0 += 4800)
        {
            std::vector<float> m (7200);
            for (int i = 0; i < 7200; ++i) m[(size_t) i] = out.getSample (0, a0 + i);
            const float hz = analyseStem (m.data(), 7200, rate).pitchHz;
            if (hz <= 0) continue;
            ++total;
            const float semis = 12.0f * std::log2 (hz / 440.0f);
            const float within = semis - 12.0f * std::floor (semis / 12.0f);
            const int steps[] = { 0, 2, 3, 5, 7, 8, 10, 12 };
            float best = 99; for (int st : steps) best = juce::jmin (best, std::abs (within - (float) st));
            if (best < 0.35f) ++onScale;
        }
        std::printf ("       A minor: %d of %d windows on a scale note\n", onScale, total);
        CHECK (total > 10 && onScale >= total * 8 / 10, "Tune Minor: the voice lands on A-minor notes");

        // Encoded voice: the codec retunes too.
        set ("tuneScale", 0.0f); set ("tuneRoot", 0.0f); set ("encodeCount", 1.0f);
        float mc, sc;
        trackCents (130.81f, mc, sc);
        std::printf ("       encoded, Tune 100%% Root: spread %.0f cents, avg %.0f off C3\n", sc, mc);
        CHECK (sc < 60.0f && mc < 40.0f, "encoded voices are retuned by the codec");
    }


    std::printf ("recording from the sidechain\n");
    {
        DrosakaVoiceProcessor p; p.prepareToPlay (rate, 256);
        const auto takes = tmp.getChildFile ("takes");
        p.setTakesFolder (takes);
        CHECK (p.getBusCount (true) == 2 && p.getBus (true, 1)->isEnabled() && p.getBus (true, 1)->getName() == "Sidechain",
               "the device has a Sidechain input (Live's sidechain menu feeds it)");
        CHECK (p.getBus (true, 0)->isEnabled() == false && p.getBus (false, 0)->getNumberOfChannels() == 2, "still an instrument: no main input, stereo out");

        auto set = [&] (const char* id, float v) { auto* prm = p.params.getParameter (id); prm->setValueNotifyingHost (prm->convertTo0to1 (v)); };
        // Feed a signal into the sidechain channels (what Live hands the plugin) block by block.
        auto feed = [&] (const juce::AudioBuffer<float>& src, int startSample, int blocks)
        {
            juce::AudioBuffer<float> blk (2, 256); juce::MidiBuffer m;
            for (int k = 0; k < blocks; ++k)
            {
                blk.clear();
                for (int i = 0; i < 256; ++i)
                {
                    const int at = startSample + k * 256 + i;
                    const float x = at < src.getNumSamples() ? src.getSample (0, at) : 0.0f;
                    blk.setSample (0, i, x); blk.setSample (1, i, x);
                }
                p.processBlock (blk, m);
            }
        };
        // A take: 0.5 s of silence, 1.2 s of voice, 0.4 s of silence.
        auto voice = voiceTone (160.0f, 0.4f, rate, (int) (rate * 1.2));
        juce::AudioBuffer<float> takeSig (1, (int) (rate * 2.1)); takeSig.clear();
        takeSig.copyFrom (0, (int) (rate * 0.5), voice, 0, 0, voice.getNumSamples());

        set ("record", 1.0f);
        feed (takeSig, 0, (int) (rate * 2.1) / 256);
        CHECK (p.recordingNow.load() && p.recordSeconds.load() > 2.0f, "REC on: the sidechain is being captured");
        set ("record", 0.0f);
        feed (takeSig, 0, 2);
        p.processFinishedTake();
        auto b = p.getBank();
        CHECK (b->size() == 1, "REC off: the take is added to the bank");
        const double secs = b->stems[0]->audio.getNumSamples() / b->stems[0]->sampleRate;
        std::printf ("       take trimmed to %.2f s (1.2 s of voice); %s\n", secs, p.getRecordMessage().toRawUTF8());
        CHECK (secs > 1.2 && secs < 1.4, "silence before and after is trimmed off");
        CHECK (std::abs (b->stems[0]->analysis.pitchHz - 160.0f) < 3.0f, "it's the recorded voice");
        CHECK (takes.getNumberOfChildFiles (juce::File::findFiles, "*.wav") == 1, "and saved as a WAV in the takes folder");

        // A second take, sung later in its recording: auto-aligned to the first.
        auto voice2 = voiceTone (220.0f, 0.4f, rate, (int) (rate * 1.2));
        for (int i = 0; i < voice2.getNumSamples(); ++i)   // same syllable rhythm as the first take
        {
            const double t = i / rate, ph = std::fmod (t, 0.3);
            const float env = ph < 0.2 ? (float) std::sin (juce::MathConstants<double>::pi * ph / 0.2) : 0.0f;
            voice2.setSample (0, i, voice2.getSample (0, i) * env);
            voice.setSample (0, i, voice.getSample (0, i) * env);
        }
        // redo take 1 with the same rhythm so both share it
        p.removeStem (0);
        takeSig.clear(); takeSig.copyFrom (0, (int) (rate * 0.5), voice, 0, 0, voice.getNumSamples());
        set ("record", 1.0f); feed (takeSig, 0, (int) (rate * 2.1) / 256); set ("record", 0.0f); feed (takeSig, 0, 2); p.processFinishedTake();
        juce::AudioBuffer<float> take2 (1, (int) (rate * 2.5)); take2.clear();
        take2.copyFrom (0, (int) (rate * 0.9), voice2, 0, 0, voice2.getNumSamples());
        set ("record", 1.0f); feed (take2, 0, (int) (rate * 2.5) / 256); set ("record", 0.0f); feed (take2, 0, 2); p.processFinishedTake();
        b = p.getBank();
        std::printf ("       second take: offset %.1f ms, alignment %d\n", b->size() > 1 ? b->flags[1].offsetSec * 1000 : 0.0, b->size() > 1 ? (int) b->flags[1].align : -1);
        CHECK (b->size() == 2 && b->flags[1].align == AlignState::autoAligned && std::abs (b->flags[1].offsetSec) < 0.01,
               "a second take is auto-aligned to the first (both trimmed to their first sound)");

        // Nothing routed in: silence -> nothing added, and a hint.
        juce::AudioBuffer<float> silence (1, (int) rate); silence.clear();
        set ("record", 1.0f); feed (silence, 0, 100); set ("record", 0.0f); feed (silence, 0, 2); p.processFinishedTake();
        CHECK (p.getBank()->size() == 2 && p.getRecordMessage().contains ("Sidechain"), "a silent take adds nothing and says how to route audio in");

        // The take buffer fills (60 s): recording stops itself and the switch turns off.
        juce::AudioBuffer<float> longSig (1, (int) (rate * 61)); longSig.clear();
        for (int i = 0; i < longSig.getNumSamples(); ++i) longSig.setSample (0, i, 0.2f * std::sin (juce::MathConstants<float>::twoPi * 150.0f * (float) i / (float) rate));
        set ("record", 1.0f); feed (longSig, 0, (int) (rate * 61) / 256); p.processFinishedTake();
        CHECK (p.params.getParameter ("record")->getValue() < 0.5f && p.getBank()->size() == 3, "a full 60 s take stops itself and is kept");

        // Takes are embedded in the set like any stem.
        juce::MemoryBlock mb; p.getStateInformation (mb);
        takes.deleteRecursively();
        DrosakaVoiceProcessor q; q.setStateInformation (mb.getData(), (int) mb.getSize());
        CHECK (q.getBank()->size() == 3, "recorded takes travel with the set (even if the WAVs are gone)");
    }


    std::printf ("recording: any input layout\n");
    {
        // Some hosts feed the main input instead of the sidechain: record whatever arrives.
        DrosakaVoiceProcessor p;
        auto layout = p.getBusesLayout();
        layout.inputBuses.getReference (0) = juce::AudioChannelSet::stereo();
        layout.inputBuses.getReference (1) = juce::AudioChannelSet::disabled();
        CHECK (p.setBusesLayout (layout), "a host can enable the main input instead");
        p.prepareToPlay (rate, 256);
        p.setTakesFolder (tmp.getChildFile ("takes2"));
        auto voice = voiceTone (180.0f, 0.4f, rate, (int) rate);
        juce::AudioBuffer<float> blk (2, 256); juce::MidiBuffer m;
        auto* rec = p.params.getParameter ("record");
        rec->setValueNotifyingHost (1.0f);
        for (int k = 0; k < (int) rate / 256; ++k)
        {
            for (int i = 0; i < 256; ++i) { blk.setSample (0, i, voice.getSample (0, k * 256 + i)); blk.setSample (1, i, voice.getSample (0, k * 256 + i)); }
            p.processBlock (blk, m);
        }
        CHECK (p.inputChannels.load() == 2 && p.inputLevel.load() > 0.05f, "the input meter shows audio arriving, before anything is kept");
        rec->setValueNotifyingHost (0.0f);
        blk.clear(); p.processBlock (blk, m);
        p.processFinishedTake();
        CHECK (p.getBank()->size() == 1, "and the take is recorded from the main input");

        // No input channels at all: the message says so precisely.
        DrosakaVoiceProcessor q;
        auto l2 = q.getBusesLayout();
        l2.inputBuses.getReference (0) = juce::AudioChannelSet::disabled();
        l2.inputBuses.getReference (1) = juce::AudioChannelSet::disabled();
        q.setBusesLayout (l2);
        q.prepareToPlay (rate, 256);
        q.params.getParameter ("record")->setValueNotifyingHost (1.0f);
        for (int k = 0; k < 50; ++k) q.processBlock (blk, m);
        q.params.getParameter ("record")->setValueNotifyingHost (0.0f);
        q.processBlock (blk, m);
        q.processFinishedTake();
        std::printf ("       %s\n", q.getRecordMessage().toRawUTF8());
        CHECK (q.getRecordMessage().contains ("isn't sending"), "with no input at all, it says Live isn't sending audio (not just 'silent')");
    }

    std::printf ("banks\n");
    {
        auto fA = writeWav ("bk_a.wav", voiceTone (140.0f, 0.4f, rate, (int) (rate * 2.0)), rate);
        auto fB = writeWav ("bk_b.wav", voiceTone (220.0f, 0.4f, rate, (int) (rate * 1.2)), rate);
        auto fC = writeWav ("bk_c.wav", voiceTone (260.0f, 0.4f, rate, (int) (rate * 1.2)), rate);
        DrosakaVoiceProcessor p; p.prepareToPlay (rate, 256);
        auto set = [&] (const char* id, float v) { auto* prm = p.params.getParameter (id); prm->setValueNotifyingHost (prm->convertTo0to1 (v)); };
        auto get = [&] (const char* id) { return p.params.getRawParameterValue (id)->load(); };
        auto host = [&] (const char* id) { auto* prm = p.params.getParameter (id); return prm->convertFrom0to1 (prm->getValue()); };
        juce::StringArray errs;
        p.addStems ({ fA.getFullPathName() }, errs);
        p.addSlice (0.3); p.addSlice (1.0);
        set ("spread", 0.2f); set ("scheme", (float) pattern::anchor); set ("playMode", 1.0f);
        CHECK (p.getActiveBank() == 0 && p.params.getParameter ("bank") != nullptr, "one instance starts on bank 1");

        // Stopped transport: a bank change happens at once.
        set ("bank", 1.0f);
        CHECK (p.getActiveBank() == 0, "nothing changes until the audio runs");
        run (p, 256, 1, {});
        CHECK (p.getActiveBank() == 1 && p.getBank()->size() == 0 && p.getSlices().n == 0, "stopped: the switch is immediate, to an empty bank 2");
        CHECK (std::abs (get ("spread") - 0.2f) < 1e-5f && (int) get ("scheme") == pattern::anchor, "a bank used for the first time carries on with the current settings");

        p.addStems ({ fB.getFullPathName(), fC.getFullPathName() }, errs);
        p.addSlice (0.5);
        set ("spread", 0.9f); set ("scheme", (float) pattern::rotate); set ("playMode", 0.0f);
        CHECK (p.getBank()->size() == 2 && p.getBankStemCount (0) == 1 && p.getBankStemCount (1) == 2, "stems load into the bank that's playing");

        set ("bank", 0.0f); run (p, 256, 1, {});
        p.flushBankRecall();
        CHECK (p.getActiveBank() == 0 && p.getBank()->size() == 1 && near (sliceTimes (p), { 0.3, 1.0 }), "back to bank 1: its stems and markers");
        CHECK (std::abs (get ("spread") - 0.2f) < 1e-5f && (int) get ("scheme") == pattern::anchor && get ("playMode") > 0.5f,
               "and its settings, Play mode included");
        CHECK (std::abs (host ("spread") - 0.2f) < 1e-4f && (int) std::lround (host ("scheme")) == pattern::anchor, "the host is told (so Live updates your controller)");

        set ("spread", 0.33f);   // a tweak, then away and back: it's kept
        set ("bank", 1.0f); run (p, 256, 1, {}); p.flushBankRecall();
        CHECK (std::abs (get ("spread") - 0.9f) < 1e-5f && (int) get ("scheme") == pattern::rotate && get ("playMode") < 0.5f && near (sliceTimes (p), { 0.5 }),
               "bank 2 comes back as it was left");
        set ("bank", 0.0f); run (p, 256, 1, {}); p.flushBankRecall();
        CHECK (std::abs (get ("spread") - 0.33f) < 1e-5f, "tweaks are kept when you leave a bank and come back");

        set ("drop", 1.0f); set ("bank", 1.0f); run (p, 256, 1, {}); p.flushBankRecall();
        CHECK (get ("drop") > 0.5f, "DROP is global: switching banks doesn't lift it");
        set ("drop", 0.0f);

        // Playing: the switch waits for the bar line, then lands exactly on it.
        TestPlayHead ph; ph.bpm = 120.0; ph.ppq = 0.5; p.setPlayHead (&ph);
        set ("bank", 0.0f); set ("playMode", 0.0f);   // (bank 2 plays by hand)
        const double spb = rate * 60.0 / ph.bpm;
        const int L = p.getLatencySamples();
        juce::AudioBuffer<float> blk (2, 256);
        int switchedBlock = -1;
        for (int k = 0; k < 700; ++k)
        {
            juce::MidiBuffer m;
            p.processBlock (blk, m);
            if (switchedBlock < 0 && p.getActiveBank() == 0) switchedBlock = k;
            ph.ppq += 256.0 / spb;
        }
        const int barSample = (int) std::round ((4.0 - 0.5) * spb);   // beat 4.0, from 0.5
        std::printf ("       switch requested at beat 0.5, landed in block %d (the bar is in block %d)\n", switchedBlock, barSample / 256);
        CHECK (switchedBlock == barSample / 256, "playing: the switch waits for the next bar");
        p.setPlayHead (nullptr);

        // A phrase sounding across the line: bank 2's sound stops right at the bar.
        set ("bank", 1.0f); run (p, 256, 2, {});
        set ("playMode", 0.0f);
        TestPlayHead ph2; ph2.bpm = 120.0; ph2.ppq = 3.0; p.setPlayHead (&ph2);   // bank 2's 1.2 s phrase from beat 3 runs past beat 4
        set ("bank", 0.0f);
        juce::AudioBuffer<float> o2 (2, 256 * 300);
        for (int k = 0; k < 300; ++k)
        {
            juce::MidiBuffer m; if (k == 0) m.addEvent (juce::MidiMessage::noteOn (1, 30, (juce::uint8) 100), 0);
            p.processBlock (blk, m); ph2.ppq += 256.0 / spb;
            for (int ch = 0; ch < 2; ++ch) o2.copyFrom (ch, k * 256, blk, ch, 0, 256);
        }
        const int line = (int) std::round (1.0 * spb) + L;   // beat 4, heard after the latency
        const float before = juce::jmax (o2.getMagnitude (0, line - 2400, 2000), o2.getMagnitude (1, line - 2400, 2000));
        const float after = juce::jmax (o2.getMagnitude (0, line + 400, 2000), o2.getMagnitude (1, line + 400, 2000));
        std::printf ("       bank 2's phrase across the line: before %.3f, just after %.5f\n", before, after);
        CHECK (before > 0.02f && after < 1e-3f, "the old bank's sound fades out exactly at the bar line");
        p.setPlayHead (nullptr);

        // Save and reload: every bank, its markers and its settings.
        set ("spread", 0.77f);   // bank 1 is playing now
        juce::MemoryBlock mb; p.getStateInformation (mb);
        DrosakaVoiceProcessor q; q.prepareToPlay (rate, 256);
        q.setStateInformation (mb.getData(), (int) mb.getSize());
        auto getq = [&] (const char* id) { return q.params.getRawParameterValue (id)->load(); };
        CHECK (q.getActiveBank() == 0 && q.getBank()->size() == 1 && q.getBankStemCount (1) == 2 && std::abs (getq ("spread") - 0.77f) < 1e-4f,
               "a saved set brings back every bank, playing the one it was on");
        auto setq = [&] (const char* id, float v) { auto* prm = q.params.getParameter (id); prm->setValueNotifyingHost (prm->convertTo0to1 (v)); };
        setq ("bank", 1.0f); run (q, 256, 1, {}); q.flushBankRecall();
        CHECK (q.getBank()->size() == 2 && near (sliceTimes (q), { 0.5 }) && std::abs (getq ("spread") - 0.9f) < 1e-4f && (int) getq ("scheme") == pattern::rotate,
               "and the other banks' stems, markers and settings");
        bool embedded = true; for (auto& st : q.getBank()->stems) embedded &= st->flac.getSize() > 0;
        CHECK (embedded, "with their audio embedded");

        // A latched pattern carries across: it restarts from the new phrase's first slice on the line.
        setq ("playMode", 1.0f); setq ("latch", 1.0f);           // bank 2: Pattern, three slices
        q.addSlice (0.2); q.addSlice (0.8);
        setq ("bank", 0.0f); run (q, 256, 1, {}); q.flushBankRecall();
        setq ("playMode", 1.0f); setq ("latch", 1.0f); setq ("scheme", 0.0f); setq ("patternDiv", 1.0f);   // bank 1: Pattern, Phrase, 1/8
        TestPlayHead ph3; ph3.bpm = 120.0; ph3.ppq = 0.0; q.setPlayHead (&ph3);
        juce::AudioBuffer<float> b3 (2, 256);
        auto step = [&] (int blocks, int noteOn = -1, int noteOff = -1)
        {
            for (int k = 0; k < blocks; ++k)
            {
                juce::MidiBuffer m;
                if (k == 0 && noteOn >= 0) m.addEvent (juce::MidiMessage::noteOn (1, noteOn, (juce::uint8) 100), 0);
                if (k == 0 && noteOff >= 0) m.addEvent (juce::MidiMessage::noteOff (1, noteOff), 0);
                q.processBlock (b3, m); ph3.ppq += 256.0 / spb;
            }
        };
        step (40, 61); step (1, -1, 61);   // C#3: the loop starts on slice 2
        step (20);
        const bool walking = q.patternActive.load() && q.patternSlice.load() >= 0;
        setq ("bank", 1.0f);
        while (ph3.ppq < 4.0 - 1e-9) step (1);
        step (12);   // just past the line: the first hit of the new phrase
        std::printf ("       pattern after the switch: active %d, slice %d (of bank 2's %d)\n", (int) q.patternActive.load(), q.patternSlice.load(), q.getSlices().numRegions());
        CHECK (walking && q.getActiveBank() == 1 && q.patternActive.load() && q.patternSlice.load() == 0,
               "a latched pattern keeps going across the switch, from the new phrase's first slice");
        q.setPlayHead (nullptr);

    }

    std::printf ("stem gain trim\n");
    {
        auto fA = writeWav ("gt_a.wav", voiceTone (140.0f, 0.4f, rate, (int) (rate * 1.5)), rate);
        auto fB = writeWav ("gt_b.wav", voiceTone (200.0f, 0.4f, rate, (int) (rate * 1.5)), rate);
        DrosakaVoiceProcessor p; p.prepareToPlay (rate, 256);
        juce::StringArray errs; p.addStems ({ fA.getFullPathName(), fB.getFullPathName() }, errs);
        p.setSolo (1, true);
        auto level = [&]
        {
            run (p, 256, 400, {});
            auto o = run (p, 256, 200, { 0 });
            return o.getRMSLevel (0, 4800, 40000) + o.getRMSLevel (1, 4800, 40000);
        };
        const float at0 = level();
        p.setStemGain (1, -6.0f);
        const float atM6 = level();
        p.setStemGain (1, 6.0f);
        const float atP6 = level();
        std::printf ("       -6 dB: %+.2f dB, +6 dB: %+.2f dB\n", 20 * std::log10 (atM6 / at0), 20 * std::log10 (atP6 / at0));
        CHECK (std::abs (20 * std::log10 (atM6 / at0) + 6.0) < 0.2 && std::abs (20 * std::log10 (atP6 / at0) - 6.0) < 0.2, "a stem's gain trim sets its level");
        p.setStemGain (1, 40.0f);
        CHECK (p.getBank()->flags[1].gainDb == 12.0f, "trim is limited to -24..+12 dB");
        p.setStemGain (1, -4.5f);
        juce::MemoryBlock mb; p.getStateInformation (mb);
        DrosakaVoiceProcessor q; q.prepareToPlay (rate, 256); q.setStateInformation (mb.getData(), (int) mb.getSize());
        CHECK (std::abs (q.getBank()->flags[1].gainDb + 4.5f) < 1e-4f && q.getBank()->flags[0].gainDb == 0.0f, "trims save with the set");
        auto setq = [&] (const char* id, float v) { auto* prm = q.params.getParameter (id); prm->setValueNotifyingHost (prm->convertTo0to1 (v)); };
        setq ("bank", 1.0f); run (q, 256, 1, {});
        q.addStems ({ fA.getFullPathName() }, errs);
        q.setStemGain (0, 3.0f);
        setq ("bank", 0.0f); run (q, 256, 1, {});
        CHECK (std::abs (q.getBank()->flags[1].gainDb + 4.5f) < 1e-4f && q.getBank()->flags[0].gainDb == 0.0f, "each bank keeps its own trims");
    }

    std::printf ("stem outputs\n");
    {
        auto fA = writeWav ("so_a.wav", voiceTone (140.0f, 0.4f, rate, (int) (rate * 1.5)), rate);
        auto fB = writeWav ("so_b.wav", voiceTone (200.0f, 0.4f, rate, (int) (rate * 1.5)), rate);
        DrosakaVoiceProcessor p;
        CHECK (p.getBusCount (false) == 1 + numStemOuts && p.getBus (false, 2)->getName() == "Stem 2" && p.getTotalNumOutputChannels() == 2 + 2 * numStemOuts,
               "a main stereo out plus 16 stereo stem outputs");
        p.prepareToPlay (rate, 256);
        juce::StringArray errs; p.addStems ({ fA.getFullPathName(), fB.getFullPathName() }, errs);
        setCount (p, 1.0f);
        const int chans = p.getTotalNumOutputChannels();
        auto render = [&]
        {
            juce::AudioBuffer<float> blk (chans, 256), out (chans, 256 * 200);
            for (int k = 0; k < 400; ++k) { juce::MidiBuffer m; blk.clear(); p.processBlock (blk, m); }
            for (int k = 0; k < 200; ++k)
            {
                juce::MidiBuffer m; if (k == 0) m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
                blk.clear(); p.processBlock (blk, m);
                for (int ch = 0; ch < chans; ++ch) out.copyFrom (ch, k * 256, blk, ch, 0, 256);
            }
            return out;
        };
        auto rms = [] (const juce::AudioBuffer<float>& o, int ch) { return o.getRMSLevel (ch, 4800, 40000); };
        auto pitchOn = [&] (const juce::AudioBuffer<float>& o, int ch)
        {
            juce::AudioBuffer<float> m (1, 40000);
            for (int i = 0; i < 40000; ++i) m.setSample (0, i, o.getSample (ch, 4800 + i) + o.getSample (ch + 1, 4800 + i));
            return analyseStem (m.getReadPointer (0), 40000, rate).pitchHz;
        };
        auto both = render();
        const float mainBoth = rms (both, 0) + rms (both, 1);
        CHECK (rms (both, 4) + rms (both, 5) < 1e-6f, "off by default: every stem in the main mix, stem outputs silent");
        p.setStemDirect (1, true);
        auto split = render();
        const float main = rms (split, 0) + rms (split, 1), own = rms (split, 4) + rms (split, 5);
        const float mainPitch = pitchOn (split, 0), ownPitch = pitchOn (split, 4);
        std::printf ("       main %.3f (was %.3f), Stem 2 out %.3f; main pitch %.0f Hz, Stem 2 pitch %.0f Hz\n", main, mainBoth, own, mainPitch, ownPitch);
        CHECK (own > 0.02f && std::abs (ownPitch - 200.0f) < 3.0f, "a stem switched to its own output plays there");
        CHECK (std::abs (mainPitch - 140.0f) < 3.0f && main < mainBoth, "and leaves the main mix");
        juce::MemoryBlock mb; p.getStateInformation (mb);
        DrosakaVoiceProcessor q; q.prepareToPlay (rate, 256); q.setStateInformation (mb.getData(), (int) mb.getSize());
        CHECK (q.getBank()->flags[1].direct && ! q.getBank()->flags[0].direct, "the routing saves with the set");
        // A host that only gives us the main pair: a direct stem falls back into the mix rather than going silent.
        juce::AudioBuffer<float> two (2, 256);
        float peak = 0.0f;
        for (int k = 0; k < 300; ++k)
        {
            juce::MidiBuffer m; if (k == 0) m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            q.processBlock (two, m); peak = juce::jmax (peak, two.getMagnitude (0, 0, 256));
        }
        CHECK (peak > 0.05f, "with no stem outputs available, nothing goes missing");
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
