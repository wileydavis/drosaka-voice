#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

// Per-stem analysis used to sort the bank (spec: "average pitch and formant centroid").
struct StemAnalysis
{
    float pitchHz = 0.0f;      // median F0 over voiced frames; 0 if nothing voiced
    float centroidHz = 0.0f;   // energy-weighted spectral centroid (200 Hz – 6 kHz) of voiced frames
    float voicedFraction = 0.0f;
};

// mono: one channel of audio at sampleRate. Runs on the message thread at import.
StemAnalysis analyseStem (const float* mono, int numSamples, double sampleRate);

// Pitch marks for PSOLA pitch shifting: one mark per glottal period in voiced
// speech (at the period's main peak), every 5 ms in unvoiced speech/silence.
struct PitchMarks
{
    std::vector<int> pos;         // source sample index of each mark
    std::vector<float> period;    // local period in source samples
    std::vector<unsigned char> voiced;
};

PitchMarks findPitchMarks (const float* mono, int numSamples, double sampleRate);
