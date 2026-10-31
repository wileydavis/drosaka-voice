#pragma once

#include <juce_core/juce_core.h>
#include <cmath>

// Pattern mode: one key starts a tempo-locked walk through the slices. Each step is
// a grid division; the pattern decides which slice(s) fire in it. Slices are fitted
// to their step (see the processor), so a loosely spoken phrase lands on the beat.
namespace pattern
{
    enum Type { walk = 0, repeat, stutter, screwed, skipRewind, ratchet, random };
    inline juce::StringArray typeNames() { return { "Walk", "Repeat", "Stutter", "Screwed", "Skip / Rewind", "Ratchet", "Random" }; }

    inline juce::StringArray divisionNames() { return { "1/4", "1/8", "1/16", "1/32", "1/4T", "1/8T", "1/16T" }; }
    inline double divisionBeats (int i)
    {
        static const double d[] = { 1.0, 0.5, 0.25, 0.125, 2.0 / 3.0, 1.0 / 3.0, 1.0 / 6.0 };
        return d[juce::jlimit (0, 6, i)];
    }

    struct Params
    {
        int type = walk;
        double division = 0.5;   // beats
        float density = 1.0f;    // Random: chance a step fires; Skip/Rewind: chance of a skip or rewind
        int repeats = 3;         // Repeat: hits per slice; Stutter: retriggers in the stutter step
        float drop = 5.0f;       // Screwed: semitones down
    };

    struct Hit
    {
        double offset = 0.0;   // beats from the start of the step
        double length = 0.0;   // beats the slice is fitted to
        int slice = 0;
        float pitch = 0.0f;    // semitones
        bool fit = false;      // stretch/squeeze exactly to 'length' (Screwed); otherwise stretch <=2x or truncate
    };

    struct State
    {
        juce::int64 step = 0;   // steps since the pattern started
        int slice = 0;          // current position in the phrase
        int jump = -1;          // a held slice key: go there on the next step
        double startPpq = 0.0;
    };

    // Deterministic per-step randomness, so the same pattern plays the same way each time.
    inline float chance (juce::int64 step, int salt)
    {
        juce::Random r ((juce::int64) (step * 2654435761LL + salt * 97 + 13));
        return r.nextFloat();
    }

    // Produces the hits for the next step and advances the state. Returns the step's
    // length in beats. regions >= 1 (no slices = the whole phrase as one slice).
    inline double nextStep (State& st, const Params& p, int regions, double stepStartPpq, Hit* hits, int& numHits)
    {
        numHits = 0;
        const int R = juce::jmax (1, regions);
        double len = p.division;

        if (st.jump >= 0) { st.slice = st.jump % R; st.jump = -1; }
        auto add = [&] (double off, double l, int sl, float pitch = 0.0f, bool fit = false)
        {
            if (numHits < 8) hits[numHits++] = { off, l, ((sl % R) + R) % R, pitch, fit };
        };
        const juce::int64 k = st.step;

        switch (p.type)
        {
            case walk:
                add (0.0, len, st.slice);
                st.slice = (st.slice + 1) % R;
                break;

            case repeat:
                add (0.0, len, st.slice);
                if ((k + 1) % juce::jmax (1, p.repeats) == 0) st.slice = (st.slice + 1) % R;
                break;

            case stutter:
                // Odd steps: the slice. Even steps after it: N retriggers of its opening, each
                // shorter as N grows - then move on.
                if (k % 2 == 0) add (0.0, len, st.slice);
                else
                {
                    const int n = juce::jmax (1, p.repeats);
                    for (int i = 0; i < n; ++i) add (len * i / n, len / n, st.slice);
                    st.slice = (st.slice + 1) % R;
                }
                break;

            case screwed:
                add (0.0, len, st.slice, -p.drop, true);
                st.slice = (st.slice + 1) % R;
                break;

            case skipRewind:
            {
                const float c = chance (k, 1);
                if (c < p.density * 0.5f)
                {
                    // silent step; the walk still moves on
                    st.slice = (st.slice + 1) % R;
                }
                else if (c < p.density)
                {
                    st.slice = (st.slice - 1 + R) % R;   // step back a slice and play it
                    add (0.0, len, st.slice);
                    st.slice = (st.slice + 1) % R;
                }
                else
                {
                    add (0.0, len, st.slice);
                    st.slice = (st.slice + 1) % R;
                }
                break;
            }

            case ratchet:
            {
                // The division halves every bar since the start, down to 1/16 of it.
                const int bars = (int) std::floor ((stepStartPpq - st.startPpq) / 4.0 + 1e-9);
                len = p.division / std::pow (2.0, juce::jlimit (0, 4, bars));
                add (0.0, len, st.slice);
                st.slice = (st.slice + 1) % R;
                break;
            }

            case random:
            {
                // A fresh shuffle of the slices every pass through them.
                const juce::int64 cycle = k / R;
                int order[64];
                const int m = juce::jmin (R, 64);
                for (int i = 0; i < m; ++i) order[i] = i;
                juce::Random rng ((juce::int64) (cycle * 7919 + 3));
                for (int i = m - 1; i > 0; --i) std::swap (order[i], order[rng.nextInt (i + 1)]);
                if (chance (k, 2) < p.density) add (0.0, len, order[(int) (k % m)]);
                break;
            }
            default: break;
        }
        ++st.step;
        return len;
    }
}
