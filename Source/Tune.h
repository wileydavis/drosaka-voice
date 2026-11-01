#pragma once

#include <juce_core/juce_core.h>
#include <cmath>

// Autotune: pull each pitch period toward a target.
//   Root  - every voice sits on the root note (in the octave nearest its own register):
//           a monotone, the most robotic setting
//   Minor / Major - each period snaps to the nearest note of that scale on the root
// Amount 0 = off, 1 = hard snap; in between pulls part of the way (ratio^amount).
namespace tune
{
    enum Scale { root = 0, minor = 1, major = 2 };
    inline juce::StringArray scaleNames() { return { "Root", "Minor", "Major" }; }
    inline juce::StringArray rootNames() { return { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }; }

    inline float rootHz (int pitchClass) { return 440.0f * std::exp2 ((float) (pitchClass - 9) / 12.0f); }   // C = C4

    // Where a period at f0 should go. 'voiceCentre' is the voice's median pitch (Root mode
    // keeps each voice in its own octave so a crowd doesn't collapse onto one note).
    inline float target (float f0, int pitchClass, int scale, float voiceCentre)
    {
        const float base = rootHz (pitchClass);
        if (scale == root)
        {
            const float c = voiceCentre > 0.0f ? voiceCentre : f0;
            return base * std::exp2 (std::round (std::log2 (c / base)));
        }
        static const int minorSteps[] = { 0, 2, 3, 5, 7, 8, 10, 12 };
        static const int majorSteps[] = { 0, 2, 4, 5, 7, 9, 11, 12 };
        const int* steps = scale == minor ? minorSteps : majorSteps;
        const float semis = 12.0f * std::log2 (f0 / base);
        const float octave = std::floor (semis / 12.0f);
        const float within = semis - 12.0f * octave;
        int best = 0;
        for (int i = 1; i < 8; ++i)
            if (std::abs (within - (float) steps[i]) < std::abs (within - (float) steps[best])) best = i;
        return base * std::exp2 ((12.0f * octave + (float) steps[best]) / 12.0f);
    }

    // Pitch ratio to apply to a period at f0.
    inline float ratio (float f0, float amount, int pitchClass, int scale, float voiceCentre)
    {
        if (amount <= 0.0f || f0 <= 0.0f) return 1.0f;
        return std::pow (target (f0, pitchClass, scale, voiceCentre) / f0, juce::jlimit (0.0f, 1.0f, amount));
    }

    struct Spec { float amount = 0.0f; int pitchClass = 0; int scale = root; };
}
