#pragma once

#include "Bank.h"
#include <cmath>

// Spatial modes and the musical grid.
//   Radiate - pairs fan out from the centre, wider with distance (the M2 layout)
//   Bounce  - on every division each voice hops to a new place in the field
//   Orbit   - voices circle the field; one revolution per division
//   Swap    - the radiate layout mirrors left <-> right on every division
// The primary always sits dead centre.
namespace spatial
{
    enum Mode { radiate = 0, bounce = 1, orbit = 2, swap = 3 };
    inline juce::StringArray modeNames() { return { "Radiate", "Bounce", "Orbit", "Swap" }; }

    // Note divisions, in beats (quarter notes).
    inline juce::StringArray divisionNames() { return { "4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16", "1/4T", "1/8T", "1/16T" }; }
    inline double divisionBeats (int i)
    {
        static const double d[] = { 16.0, 8.0, 4.0, 2.0, 1.0, 0.5, 0.25, 2.0 / 3.0, 1.0 / 3.0, 1.0 / 6.0 };
        return d[juce::jlimit (0, 9, i)];
    }

    // Encode quantise grid ("Free" = continuous).
    inline juce::StringArray quantiseNames() { return { "Free", "1/16", "1/8", "1/4", "1/2", "1 bar" }; }
    inline double quantiseBeats (int i)
    {
        static const double q[] = { 0.0, 0.25, 0.5, 1.0, 2.0, 4.0 };
        return q[juce::jlimit (0, 5, i)];
    }

    // Glide time per mode: positions jump quickly when motion is rhythmic.
    inline double panGlideSeconds (int mode)
    {
        return mode == radiate ? 0.10 : (mode == orbit ? 0.02 : 0.015);
    }

    // Target pan per stem. 'radiatePan' is computeCrowd's layout; 'audible' marks voices
    // with crowd gain > 0. 'step' is the index of the current division (bounce/swap),
    // 'cycle' the position within an orbit revolution (0..1).
    inline void pans (int mode, const Bank& b, const float* radiatePan, const float* gain,
                      juce::int64 step, double cycle, float* out)
    {
        const int n = juce::jmin (b.size(), maxVoices);
        for (int i = 0; i < n; ++i) out[i] = radiatePan[i];
        if (mode == radiate || n == 0) return;

        if (mode == swap)
        {
            const float sign = (step & 1) ? -1.0f : 1.0f;
            for (int i = 0; i < n; ++i) out[i] = radiatePan[i] * sign;
            return;
        }

        // Audible voices other than the primary, in sorted (pitch) order.
        int list[maxVoices], m = 0;
        for (int s = 0; s < n; ++s)
        {
            const int idx = b.sorted[(size_t) s];
            if (idx != b.primary && gain[idx] > 0.0f) list[m++] = idx;
        }
        if (m == 0) return;

        if (mode == bounce)
        {
            // Evenly spaced slots across the field, dealt out in a new order each division,
            // so the crowd stays spread while every voice jumps.
            float slots[maxVoices];
            for (int k = 0; k < m; ++k) slots[k] = m == 1 ? (k % 2 ? 1.0f : -1.0f) * 0.7f : -1.0f + 2.0f * (float) k / (float) (m - 1);
            juce::Random rng ((juce::int64) step * 7919 + 104729);
            for (int k = m - 1; k > 0; --k) std::swap (slots[k], slots[rng.nextInt (k + 1)]);
            if (m == 1) slots[0] = (step & 1) ? 0.7f : -0.7f;
            for (int k = 0; k < m; ++k) out[list[k]] = slots[k];
        }
        else if (mode == orbit)
        {
            for (int k = 0; k < m; ++k)
                out[list[k]] = std::sin (juce::MathConstants<float>::twoPi * (float) ((double) k / m + cycle));
        }
        if (b.primary >= 0 && b.primary < n) out[b.primary] = 0.0f;
    }
}
