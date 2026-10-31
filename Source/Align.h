#pragma once

#include <vector>

// Stem alignment by speech rhythm, not waveform: different voices (or takes) of the
// same words share their syllable envelope even when their waveforms don't correlate.
namespace align
{
    constexpr double envRate = 1000.0;   // envelope samples per second (1 ms resolution)

    // Onset-weighted log-energy envelope at 1 kHz; silence = 0.
    std::vector<float> envelope (const float* mono, int numSamples, double sampleRate);

    struct Result
    {
        double offsetSec = 0.0;   // shift to apply to 'other' so it lines up with 'ref' (+ = later)
        float score = 0.0f;       // peak normalised correlation, 0..1
        float margin = 0.0f;      // how far the peak stands above the best alternative
        bool confident = false;   // false: probably different words; leave it to the user
    };

    // refOffsetSec: where the reference currently sits; the result is absolute.
    Result findOffset (const std::vector<float>& ref, const std::vector<float>& other,
                       double refOffsetSec, double maxShiftSec = 10.0);
}
