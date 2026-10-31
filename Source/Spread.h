#pragma once

#include "Bank.h"
#include <cmath>

// Spread: one knob moves the crowd unison -> chorus -> chord -> cluster.
// Each voice has a continuous pitch path through four stations; the knob is a
// position along it (stations at 0, 1/3, 2/3, 1). Snap shapes the motion between
// stations: 0 = straight portamento, 1 = lingers on each station and rushes between.
namespace spread
{
    struct ChordSet { const char* name; int up[4]; int down[4]; };

    // Intervals in semitones from the primary, narrow -> wide. Outer pairs take the
    // widest; beyond the list, they go up/down an octave.
    inline const ChordSet* chordSets()
    {
        static const ChordSet sets[] = {
            { "Fifths",         { 7, 12, 19, 24 }, { -5, -12, -17, -24 } },
            { "Minor triad",    { 3, 7, 12, 15 },  { -5, -9, -12, -17 } },
            { "Major triad",    { 4, 7, 12, 16 },  { -5, -8, -12, -17 } },
            { "Stacked fourths",{ 5, 10, 15, 20 }, { -5, -10, -15, -20 } },
            { "Minor 7th",      { 3, 7, 10, 15 },  { -2, -5, -9, -14 } },
            { "Octaves",        { 12, 24, 12, 24 },{ -12, -24, -12, -24 } },
        };
        return sets;
    }
    constexpr int numChordSets = 6;

    constexpr float dryLimit = 7.0f;   // semitones; the PSOLA path stays within this
    constexpr float codecLimit = 24.0f;

    // Fold an interval into +-7 st by octaves (keeps the pitch class), for dry voices.
    inline float foldForDry (float semis)
    {
        while (semis > dryLimit) semis -= 12.0f;
        while (semis < -dryLimit) semis += 12.0f;
        return semis;
    }

    // Snap shaping of progress u (0..1) between two stations.
    inline float shape (float u, float snap)
    {
        const float k = snap * snap * 30.0f;   // 0 = linear .. ~30 = near-stepped
        if (k < 0.01f) return u;
        return 0.5f + 0.5f * std::tanh (k * (u - 0.5f)) / std::tanh (k * 0.5f);
    }

    // Pair index (1 = nearest) and side (-1 left/below, +1 right/above) of each stem,
    // from its distance to the primary in the sorted array (the crowd's own layout).
    inline void layout (const Bank& b, int* pair, int* side)
    {
        const int n = juce::jmin (b.size(), maxVoices);
        int p = 0;
        for (int s = 0; s < n; ++s) if (b.sorted[(size_t) s] == b.primary) { p = s; break; }
        int rankPos[maxVoices];
        int r = 0;
        rankPos[r++] = p;
        for (int k = 1; r < n; ++k)
        {
            if (p - k >= 0) rankPos[r++] = p - k;
            if (p + k < n && r < n) rankPos[r++] = p + k;
        }
        for (r = 0; r < n; ++r)
        {
            const int stem = b.sorted[(size_t) rankPos[r]];
            pair[stem] = r == 0 ? 0 : (r + 1) / 2;
            side[stem] = rankPos[r] < p ? -1 : 1;
        }
    }

    // Target interval in semitones per stem. 'codec' is the full interval (the codec can
    // play any pitch); 'dry' folds each station into +-7 st first and then glides between
    // the folded stations, so dry voices never jump an octave mid-sweep. Primary = 0.
    inline void compute (const Bank& b, float knob, int chord, float snap, float* codec, float* dry)
    {
        const int n = juce::jmin (b.size(), maxVoices);
        int pair[maxVoices], side[maxVoices];
        layout (b, pair, side);
        const auto& cs = chordSets()[juce::jlimit (0, numChordSets - 1, chord)];

        const float pos = juce::jlimit (0.0f, 1.0f, knob) * 3.0f;
        const int seg = juce::jmin (2, (int) pos);
        const float u = shape (pos - (float) seg, snap);

        for (int s = 0; s < n; ++s)
        {
            const int k = pair[s];
            if (k == 0) { codec[s] = dry[s] = 0.0f; continue; }
            const float sd = (float) side[s];

            // Stations: unison, detune (cents, irregular so it chorus-beats), chord tone, cluster.
            const float detune = sd * (0.07f + 0.05f * (float) (k - 1) + 0.01f * (float) ((k * 37) % 7 - 3));
            const int slot = (k - 1) % 4, octaves = (k - 1) / 4;
            const float chordTone = sd > 0 ? (float) (cs.up[slot] + 12 * octaves) : (float) (cs.down[slot] - 12 * octaves);
            const float cluster = sd * (float) k;
            const float st[4] = { 0.0f, juce::jlimit (-0.3f, 0.3f, detune),
                                  juce::jlimit (-codecLimit, codecLimit, chordTone),
                                  juce::jlimit (-codecLimit, codecLimit, cluster) };
            codec[s] = st[seg] + (st[seg + 1] - st[seg]) * u;
            const float a = foldForDry (st[seg]), z = foldForDry (st[seg + 1]);
            dry[s] = a + (z - a) * u;
        }
    }
}
