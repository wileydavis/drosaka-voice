#pragma once

#include <juce_core/juce_core.h>
#include <cmath>
#include <cstdint>

// Pattern mode: one key starts a tempo-locked loop through the phrase, beginning at that
// key's slice and running on through every slice (wrapping). Each step is a grid division.
//
// The SCHEME decides the order the slices come in; four shared macros shape any scheme:
//   LENGTH    how many slices the loop uses, from the trigger slice (changes at loop restarts)
//   FILL      how many of the bar's steps get a hit, spread evenly (Euclidean); the phrase
//             picks up where it left off on the next hit
//   STUTTER   rolls: a hit becomes 2/3/4/6 retriggers of the slice's opening, weighted
//             toward the end of every 4-bar block
//   VARIATION how far each bar strays from the loop (skips, repeats, jumps, reversed slices,
//             pitch drops, ghost hits); bars 4 and 8 stray most, and every 8 bars repeat
// plus SWING (late off-steps) and ACCENT (downbeats loud, off-steps quieter).
//
// Everything rhythmic is computed from the song position, so the same bars of a song always
// play the same way. Slices are fitted to their step by the processor.
namespace pattern
{
    enum Scheme { phrase = 0, anchor, offbeat, rotate, polymeter, pingPong, callAnswer, shuffle, screwed, build };
    constexpr int numSchemes = 10;
    inline juce::StringArray schemeNames()
    {
        return { "Phrase", "Anchor", "Offbeat", "Rotate", "Polymeter", "Ping-pong", "Call & answer", "Shuffle", "Screwed", "Build" };
    }
    // The pattern types of builds before Nov 1 (Walk, Repeat, Stutter, Screwed, Skip/Rewind,
    // Ratchet, Random), mapped to the nearest scheme.
    inline int schemeFromOldType (int t)
    {
        static const int m[] = { phrase, phrase, phrase, screwed, phrase, build, shuffle };
        return m[juce::jlimit (0, 6, t)];
    }

    inline juce::StringArray divisionNames() { return { "1/4", "1/8", "1/16", "1/32", "1/4T", "1/8T", "1/16T" }; }
    inline double divisionBeats (int i)
    {
        static const double d[] = { 1.0, 0.5, 0.25, 0.125, 2.0 / 3.0, 1.0 / 3.0, 1.0 / 6.0 };
        return d[juce::jlimit (0, 6, i)];
    }

    struct Params
    {
        int scheme = phrase;
        double division = 0.25;  // beats
        float length = 1.0f;     // 0..1: one slice .. all of them
        float fill = 1.0f;       // 0..1: share of the bar's steps that get a hit
        float stutter = 0.0f;
        float variation = 0.0f;
        float swing = 0.0f;      // 0..1: off-steps up to half a step late (50%..75% swing)
        float accent = 0.0f;
        float drop = 5.0f;       // Screwed: semitones down
    };

    struct Hit
    {
        double offset = 0.0;   // beats from the start of the step
        double length = 0.0;   // beats the slice is fitted to
        int slice = 0;
        float pitch = 0.0f;    // semitones
        bool fit = false;      // stretch/squeeze exactly to 'length' (Screwed); otherwise stretch <=2x or truncate
        float gain = 1.0f;
        bool reverse = false;  // play the slice backwards from its end
        float gate = 1.0f;     // multiplies the Gate knob (stabs, chops)
    };

    struct State
    {
        juce::int64 step = 0;   // steps since the pattern started
        int start = 0;          // the trigger slice: the loop begins here
        int jump = -1;          // a held slice key: restart the loop there on the next step
        double startPpq = 0.0;
        int loopLen = 0;        // slices in the loop now (Length is picked up at loop restarts)
        juce::int64 hit = 0;    // hits since the loop (re)started
        int lastIdx = 0;        // last loop position played
    };

    // Deterministic randomness from a position and a salt.
    inline float hash01 (juce::int64 a, int salt)
    {
        uint64_t x = (uint64_t) a * 0x9E3779B97F4A7C15ULL + (uint64_t) (salt + 1) * 0xBF58476D1CE4E5B9ULL + 0x94D049BB133111EBULL;
        x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 27; x *= 0x94D049BB133111EBULL;
        x ^= x >> 31;
        return (float) (x >> 40) / 16777216.0f;
    }

    // k hits spread as evenly as possible over n steps, the first on step 0.
    inline bool euclid (int i, int k, int n)
    {
        if (k >= n) return true;
        if (k <= 0) return false;
        return (i * k) % n < k;
    }

    inline juce::int64 floorDiv (double x) { return (juce::int64) std::floor (x + 1e-6); }

    // Produces the hits for the step starting at stepPpq and advances the state. Returns the
    // step's length in beats. regions >= 1 (no slices = the whole phrase as one slice).
    inline double nextStep (State& st, const Params& p, int regions, double stepPpq, Hit* hits, int& numHits)
    {
        numHits = 0;
        const int R = juce::jmax (1, regions);
        const int targetL = 1 + (int) std::lround (juce::jlimit (0.0f, 1.0f, p.length) * (float) (R - 1));
        if (st.loopLen <= 0) st.loopLen = targetL;
        if (st.jump >= 0) { st.start = st.jump % R; st.jump = -1; st.hit = 0; st.loopLen = targetL; }

        // The grid for this step.
        const juce::int64 barsIn = juce::jmax ((juce::int64) 0, floorDiv ((stepPpq - st.startPpq) / 4.0));
        double div = p.division;
        if (p.scheme == build) div = p.division / std::pow (2.0, (double) juce::jmin ((juce::int64) 4, barsIn));
        if (p.scheme == screwed) div = p.division * 2.0;
        const double len = div;
        const int N = juce::jmax (1, (int) std::lround (4.0 / div));            // steps per bar
        const juce::int64 gridIdx = (juce::int64) std::llround (stepPpq / div);
        const int i = (int) (((gridIdx % N) + N) % N);                          // step in the bar
        const juce::int64 bar = floorDiv (stepPpq / 4.0);
        const int barIn8 = (int) (((bar % 8) + 8) % 8);
        const float barWeight = barIn8 == 7 ? 3.0f : (barIn8 % 4 == 3 ? 2.0f : 1.0f);
        const double inBar = stepPpq - (double) bar * 4.0;
        const bool lastBeatOfBlock = barIn8 % 4 == 3 && inBar >= 3.0 - 1e-6;
        const double inBeat = stepPpq - std::floor (stepPpq + 1e-6);
        const bool onBeat = inBeat < 1e-4 || inBeat > 1.0 - 1e-4;
        const bool onOff = std::abs (inBeat - 0.5) < 1e-4;
        const juce::int64 seedPos = ((gridIdx % ((juce::int64) N * 8)) + (juce::int64) N * 8) % ((juce::int64) N * 8);   // repeats every 8 bars

        const int k = juce::jmax (1, (int) std::lround (juce::jlimit (0.0f, 1.0f, p.fill) * (float) N));

        // Which steps sound.
        bool pulse = false, anchorHit = false;
        float gateMul = 1.0f;
        double span = len;   // how long a hit may ring (Offbeat: its whole eighth)
        switch (p.scheme)
        {
            case offbeat:
            {
                const bool canOff = std::abs (std::fmod (0.5, div)) < 1e-6 || std::abs (std::fmod (0.5, div) - div) < 1e-6;
                if (canOff)
                {
                    const int beat = (int) std::floor (inBar + 1e-6);
                    pulse = onOff && euclid (beat, juce::jmax (1, (int) std::lround (p.fill * 4.0f)), 4);
                    span = juce::jmax (len, 0.5);
                }
                else pulse = i % 2 == 1 && euclid (i / 2, juce::jmax (1, (int) std::lround (p.fill * (float) (N / 2))), juce::jmax (1, N / 2));
                gateMul = 0.45f;   // stabs
                break;
            }
            case anchor:
                if (onBeat) pulse = anchorHit = true;
                else pulse = euclid (i, k, N);
                break;
            default:
                pulse = euclid (i, k, N);
                break;
        }

        // Loop restarts: on a bar line, once the loop has had room for all its slices
        // (Polymeter instead restarts when it runs out of slices, wherever that falls).
        const int hitsPerBar = juce::jmax (1, p.scheme == anchor ? (N > 4 ? k * (N - 4) / N : 0) : k);   // Anchor: the phrase runs between the beats
        int loopBars = 1;
        while (loopBars < 16 && loopBars * hitsPerBar < targetL) loopBars *= 2;
        const bool blockStart = i == 0 && ((bar % loopBars) + loopBars) % loopBars == 0;
        if (p.scheme != polymeter && blockStart && st.step > 0) { st.hit = 0; st.loopLen = targetL; }
        const int L = juce::jmax (1, juce::jmin (st.loopLen, R));

        auto add = [&] (double off, double l, int idx, float pitch, bool fit, float gain, bool rev, float gate)
        {
            if (numHits < 8) hits[numHits++] = { off, l, ((st.start + idx) % R + R) % R, pitch, fit, gain, rev, gate };
        };

        // Swing: off-steps of straight grids land late; the slice still ends on time.
        const bool straight = (N & (N - 1)) == 0 && N >= 8;
        const double swingOff = straight && i % 2 == 1 ? juce::jlimit (0.0f, 1.0f, p.swing) * 0.5 * div : 0.0;
        const float a = juce::jlimit (0.0f, 1.0f, p.accent);
        const float accentGain = onBeat ? 1.0f : onOff ? 1.0f - a * 0.3f : 1.0f - a * 0.55f;
        const float pv = juce::jlimit (0.0f, 0.9f, p.variation * 0.22f * barWeight);

        if (! pulse)
        {
            // Variation adds the odd ghost hit: the next slice, quiet and short.
            if (p.scheme != build && pv > 0.0f && hash01 (seedPos, 5) < pv * 0.5f)
                add (swingOff, len - swingOff, (int) ((st.hit) % L), 0.0f, false, 0.4f * accentGain, false, 0.5f);
            ++st.step;
            return len;
        }

        // Position in the loop.
        const juce::int64 h = st.hit;
        int idx = 0;
        bool chopAnswer = false;
        switch (p.scheme)
        {
            case anchor:
                idx = anchorHit || L == 1 ? 0 : 1 + (int) (h % (L - 1));
                break;
            case rotate:
                idx = (int) ((h + barsIn) % L);
                break;
            case polymeter:
                if (h >= L) { st.hit = 0; st.loopLen = targetL; }
                idx = (int) (st.hit % juce::jmax (1, juce::jmin (st.loopLen, R)));
                break;
            case pingPong:
            {
                const int period = L > 1 ? 2 * L - 2 : 1;
                const int q = (int) (h % period);
                idx = q < L ? q : period - q;
                break;
            }
            case callAnswer:
                if (((bar / loopBars) % 2 + 2) % 2 == 1) { idx = 0; chopAnswer = true; gateMul = 0.5f; }   // answer: the trigger slice, chopped
                else idx = (int) (h % L);
                break;
            case shuffle:
            {
                int order[64];
                const int m = juce::jmin (L, 64);
                for (int j = 0; j < m; ++j) order[j] = j;
                juce::Random rng ((juce::int64) (barIn8 * 7919 + st.start * 31 + 3));
                for (int j = m - 1; j > 0; --j) std::swap (order[j], order[rng.nextInt (j + 1)]);
                idx = order[(int) (h % m)];
                break;
            }
            default:   // phrase, offbeat, screwed, build
                idx = (int) (h % L);
                break;
        }
        if (! (p.scheme == anchor && anchorHit)) ++st.hit;

        // Variation: some hits change (never the anchor).
        float pitch = p.scheme == screwed ? -p.drop : 0.0f;
        bool reverse = false;
        if (! anchorHit && pv > 0.0f && hash01 (seedPos, 11) < pv)
        {
            const float c = hash01 (seedPos, 12);
            if (c < 0.22f) { ++st.step; return len; }            // skip: a rest where the slice was
            else if (c < 0.42f) idx = st.lastIdx % L;                // repeat the last slice
            else if (c < 0.6f) idx = (idx + 2) % L;              // jump ahead
            else if (c < 0.85f) reverse = true;                  // backwards
            else pitch -= 7.0f;                                  // dropped a fifth
        }
        st.lastIdx = idx;

        // Stutter: rolls, more of them toward the end of every 4 bars.
        int rolls = 1;
        const float ps = juce::jlimit (0.0f, 1.0f, p.stutter * (0.18f * barWeight + (lastBeatOfBlock ? 0.5f : 0.0f)));
        if (ps > 0.0f && hash01 (seedPos, 13) < ps)
        {
            static const int counts[] = { 2, 3, 4, 6 };
            rolls = counts[juce::jmin (3, (int) (hash01 (seedPos, 14) * (1.0f + p.stutter * 3.0f)))];
        }
        if (chopAnswer && h % 2 == 1) rolls = juce::jmax (rolls, 2);

        const bool fit = p.scheme == screwed;
        const float gain = anchorHit ? 1.0f : accentGain;
        const double hitLen = span - swingOff;
        if (rolls <= 1) add (swingOff, hitLen, idx, pitch, fit, gain, reverse, gateMul);
        else
            for (int r = 0; r < rolls; ++r)   // a roll builds up to the hit's own level
                add (swingOff + hitLen * r / rolls, hitLen / rolls, idx, pitch, false,
                     gain * (1.0f - a * 0.45f * (1.0f - (float) r / (float) (rolls - 1))), reverse, gateMul);

        ++st.step;
        return len;
    }
}
