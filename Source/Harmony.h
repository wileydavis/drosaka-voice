#pragma once

#include "Spread.h"
#include "Tune.h"
#include <cmath>

// Harmony Spread: the crowd sings real notes.
//
// The Spread knob moves every crowd voice (never the primary) along four stations:
//   natural (0)   - untouched speech
//   chorus  (1/3) - all on the root, each in the octave nearest its own register, slightly
//                   detuned and drifting, and keeping a little of its natural wobble: a choir
//                   on one note that beats and shimmers
//   chord   (2/3) - each voice on a chord tone (Chord menu on the Tune root, or the notes you
//                   hold on the harmony keys C1-B1), chosen within +-6 semitones of the root
//                   in that voice's own octave, so the shift stays small and clean
//   cluster (1)   - neighbouring steps of the Tune scale around the root
// Snap shapes the motion between stations, as before. Each pitch period is pulled toward the
// target (PSOLA for dry voices, the excitation for encoded ones), so dry and encoded voices
// sing the same notes.
namespace harmony
{
    constexpr int keyLow = 36, keyHigh = 47;   // harmony keys C1..B1: held notes = the chord
    inline bool isHarmonyKey (int note) { return note >= keyLow && note <= keyHigh; }

    struct Voice
    {
        float semis = 0.0f;       // target, semitones from the root in this voice's octave
        float pull = 0.0f;        // how far the voice's median moves onto the target (0..1)
        float strength = 0.0f;    // how much of its wobble around the target is ironed out (0..1)
        float detune = 0.0f;      // cents
        float drift = 0.0f;       // how much of the Drift knob applies (0..1)
    };

    // Fold an interval into -6..+6 semitones (same pitch class, nearest the root).
    inline float fold (float semis)
    {
        float v = std::fmod (semis, 12.0f);
        if (v > 6.0f) v -= 12.0f;
        if (v < -6.0f) v += 12.0f;
        return v;
    }

    // k-th scale step above (side > 0) or below the root, in semitones (chromatic for 'Root').
    inline float scaleStep (int k, int side, int scale)
    {
        if (scale == tune::root) return (float) (side * k);
        static const int minor[] = { 0, 2, 3, 5, 7, 8, 10 }, major[] = { 0, 2, 4, 5, 7, 9, 11 };
        const int* steps = scale == tune::minor ? minor : major;
        // Scale notes from three octaves down to three up; the root sits at index 21.
        int notes[42];
        for (int o = -3; o < 3; ++o) for (int d = 0; d < 7; ++d) notes[(o + 3) * 7 + d] = steps[d] + 12 * o;
        return (float) notes[juce::jlimit (0, 41, 21 + side * k)];
    }

    inline float rootTargetHz (int rootPc, float centre, float semis);

    // heldPc: 12 flags for pitch classes held on the harmony keys (relative to C), or null.
    inline void compute (const Bank& b, float knob, int chord, float snap, int rootPc, int scale,
                         const bool* heldPc, Voice* out)
    {
        const int n = juce::jmin (b.size(), maxVoices);
        int pair[maxVoices], side[maxVoices];
        spread::layout (b, pair, side);

        int held[12], numHeld = 0;
        if (heldPc != nullptr)
            for (int pc = 0; pc < 12; ++pc) if (heldPc[pc]) held[numHeld++] = pc;

        // Held keys lift the knob to at least the chord station.
        float pos = juce::jlimit (0.0f, 1.0f, knob) * 3.0f;
        if (numHeld > 0) pos = juce::jmax (pos, 2.0f);
        const int seg = juce::jmin (2, (int) pos);
        const float u = spread::shape (pos - (float) seg, snap);

        // Crowd voices in pitch order (for spreading held notes low-to-high).
        int crowd[maxVoices], m = 0;
        for (int s = 0; s < n; ++s) if (b.sorted[(size_t) s] != b.primary) crowd[m++] = b.sorted[(size_t) s];
        int crowdRank[maxVoices] {};
        for (int i = 0; i < m; ++i) crowdRank[crowd[i]] = i;

        const auto& cs = spread::chordSets()[juce::jlimit (0, spread::numChordSets - 1, chord)];
        for (int s = 0; s < n; ++s)
        {
            out[s] = {};
            if (s == b.primary) continue;   // the real voice stays human
            const int k = juce::jmax (1, pair[s]);
            const int sd = side[s];

            // Chord tone: held notes spread across the crowd low-to-high, or the menu chord
            // (outer pairs take the wider tones).
            float chordTone;
            if (numHeld > 0)
                chordTone = (float) (held[juce::jmin (numHeld - 1, crowdRank[s] * numHeld / juce::jmax (1, m))] - rootPc);
            else
            {
                const int slot = (k - 1) % 4;
                chordTone = (float) (sd > 0 ? cs.up[slot] : cs.down[slot]);
            }
            const float clusterTone = scaleStep (k, sd, scale);

            // Where the voice sits now, relative to the root in its own octave (-6..+6), and
            // each tone voiced in the octave nearest that, so no voice moves more than 6 st.
            const float centre = b.stems[(size_t) s]->analysis.pitchHz;
            const float self = centre > 0.0f ? 12.0f * std::log2 (centre / rootTargetHz (rootPc, centre, 0.0f)) : 0.0f;
            auto near = [self] (float tone) { return tone - 12.0f * std::round ((tone - self) / 12.0f); };

            // Stations: natural, chorus (root), chord, cluster.
            const float semis[4]    = { self, near (0.0f), near (chordTone), near (clusterTone) };
            const float pull[4]     = { 0.0f, 1.0f, 1.0f, 1.0f };
            const float strength[4] = { 0.0f, 0.85f, 1.0f, 1.0f };
            const float detuneAmt   = (float) sd * (8.0f + 6.0f * (float) (k - 1) + (float) ((k * 37) % 7 - 3));
            const float detune[4]   = { 0.0f, detuneAmt, detuneAmt * 0.25f, detuneAmt * 0.25f };
            const float drift[4]    = { 0.0f, 1.0f, 0.5f, 0.5f };

            out[s].semis    = semis[seg] + (semis[seg + 1] - semis[seg]) * u;
            out[s].pull     = pull[seg] + (pull[seg + 1] - pull[seg]) * u;
            out[s].strength = strength[seg] + (strength[seg + 1] - strength[seg]) * u;
            out[s].detune   = detune[seg] + (detune[seg + 1] - detune[seg]) * u;
            out[s].drift    = drift[seg] + (drift[seg + 1] - drift[seg]) * u;
        }
    }

    // A slow, smooth, per-voice wander in cents (two incommensurate sines per voice).
    inline float driftCents (int voice, double seconds, float amount)
    {
        const double r1 = 0.11 + 0.037 * (voice % 7), r2 = 0.29 + 0.053 * ((voice * 3) % 5);
        const double p1 = voice * 1.7, p2 = voice * 0.9 + 2.1;
        return amount * 28.0f * (float) (0.65 * std::sin (juce::MathConstants<double>::twoPi * r1 * seconds + p1)
                                       + 0.35 * std::sin (juce::MathConstants<double>::twoPi * r2 * seconds + p2));
    }

    // Everything needed to decide where one pitch period should go.
    struct PitchMap
    {
        float targetHz = 0.0f;   // harmony target (absolute), when pull > 0
        float pull = 0.0f;       // median shift onto the target
        float strength = 0.0f;   // correction of the wobble around it
        float cents = 0.0f;      // detune + drift
        tune::Spec tune;         // the Tune knob (applies when there's no harmony, and to the primary)
        float centre = 0.0f;     // the voice's median pitch
    };

    inline float rootTargetHz (int rootPc, float centre, float semis)
    {
        const float base = tune::rootHz (rootPc);
        const float c = centre > 0.0f ? centre : base;
        return base * std::exp2 (std::round (std::log2 (c / base))) * std::exp2 (semis / 12.0f);
    }

    // Pitch ratio for a period at f0: the voice's median moves onto the target (pull), and the
    // period's deviation from the median is ironed out by strength (or by Tune, if stronger).
    inline float ratio (float f0, const PitchMap& m)
    {
        if (f0 <= 0.0f) return 1.0f;
        if (m.pull > 0.0f && m.targetHz > 0.0f && m.centre > 0.0f)
        {
            const float t = m.targetHz * std::exp2 (m.cents / 1200.0f);
            const float correct = juce::jlimit (0.0f, 1.0f, juce::jmax (m.strength, m.tune.amount));
            const float tuned = tune::ratio (f0, m.tune.amount, m.tune.pitchClass, m.tune.scale, m.centre);
            const float harm = std::pow (t / m.centre, m.pull) * std::pow (m.centre / f0, correct);
            // Fade from Tune's own result to the harmony as pull comes in.
            return tuned * std::pow (harm / tuned, juce::jlimit (0.0f, 1.0f, m.pull));
        }
        return tune::ratio (f0, m.tune.amount, m.tune.pitchClass, m.tune.scale, m.centre);
    }
}
