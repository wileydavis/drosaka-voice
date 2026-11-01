#pragma once

#include "Bank.h"
#include <cmath>

// Spread building blocks: the chord sets, the crowd layout (pairs either side of the
// primary) and Snap's shaping. The harmony itself is in Harmony.h.
namespace spread
{
    struct ChordSet { const char* name; int up[4]; int down[4]; };

    // Chord tones in semitones from the Tune root. Each crowd voice sings its tone in the
    // octave nearest its own register, so these name pitch classes (outer pairs take the
    // later ones).
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
}
