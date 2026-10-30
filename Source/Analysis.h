#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

// Per-stem analysis used to sort the bank (spec: "average pitch and formant centroid").
struct StemAnalysis
{
    float pitchHz = 0.0f;      // median F0 over voiced frames; 0 if nothing voiced
    float centroidHz = 0.0f;   // energy-weighted spectral centroid (200 Hz – 6 kHz) of voiced frames
    float voicedFraction = 0.0f;
};

// mono: one channel of audio at sampleRate. Runs on the message thread at import.
StemAnalysis analyseStem (const float* mono, int numSamples, double sampleRate);
