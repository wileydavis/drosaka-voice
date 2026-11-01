#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include "Harmony.h"

// Military-codec voice: an LPC-10-style vocoder running at 8 kHz.
//   analysis:  pre-emphasis, 240-sample Hamming window every 180 samples (22.5 ms),
//              10th-order LPC (Levinson), autocorrelation pitch + voicing
//   transport: reflection coefficients quantised in the arcsine domain, gain in dB
//              steps, pitch in semitone steps; frames can be dropped (packet loss)
//   synthesis: impulse-train (voiced) or noise (unvoiced) through an all-pole lattice
// Streaming, host-rate in and out. Fixed latency, reported by latencySamples().
class LpcCodec
{
public:
    enum class PitchLock { fixed = 0, free = 1, tracking = 2 };

    static constexpr double codecRate = 8000.0;
    static constexpr int frameLen = 180;
    static constexpr int winLen = 240;
    static constexpr int order = 10;

    void prepare (double hostRate, juce::uint32 seed);
    void reset();

    // bitRate: 0 = clean .. 1 = barely intelligible. frameDrop: 0 .. 1 (0 .. 50 % loss, bursty).
    void setParams (float bitRate, float frameDrop, PitchLock lock, float fixedPitchHz);

    // Spread: transpose the excitation (the LPC envelope, i.e. the formants, stays put).
    void setPitchRatio (float r) { pitchRatio = juce::jlimit (0.25f, 4.0f, r); }

    // Where each frame's pitch should go: harmony target (Spread) or autotune (Tune).
    void setPitchMap (const harmony::PitchMap& m) { pitchMap = m; }

    void process (const float* in, float* out, int numSamples);

    // Host-rate delay from input to output; the dry path is delayed by the same amount.
    static int latencySamples (double hostRate);

private:
    struct Frame
    {
        std::array<float, order + 1> k {};   // reflection coefficients, 1-based
        float gain = 0.0f;
        float pitchHz = 0.0f;
        bool voiced = false;
    };

    void push8k (float x);
    void runFrame();
    Frame analyse();
    void quantise (Frame&) const;
    void synthesise (const Frame& from, const Frame& to);

    double hostRate = 48000.0;
    juce::IIRFilter downLp[3];
    juce::IIRFilter outHp;   // DC blocker (see prepare)
    double downPos = 0.0;    // next 8 kHz sample position, relative to current host sample
    float downPrev = 0.0f;

    // 8 kHz input history (pre-emphasised for LPC, raw for pitch).
    static constexpr int histLen = 512;
    std::array<float, histLen> histEmph {}, histRaw {};
    int histPos = 0;
    float preEmphPrev = 0.0f;
    int sinceFrame = 0;

    // 8 kHz output FIFO.
    static constexpr int fifoLen = 1024;
    std::array<float, fifoLen> fifo {};
    int fifoRead = 0, fifoWrite = 0;
    double upFrac = 0.0;
    float deEmphPrev = 0.0f;

    // Synthesis state.
    std::array<float, order + 1> lattice {};
    double pulsePhase = 0.0;
    Frame prevFrame, lastGood;
    int frameCounter = 0, lossRun = 0;
    bool prevLost = false;
    float trackedPitch = 0.0f;
    std::array<float, 3> pitchHistory {};

    // Parameters.
    float bitRate = 0.35f, frameDrop = 0.0f, fixedPitch = 110.0f, pitchRatio = 1.0f;
    harmony::PitchMap pitchMap;
    PitchLock lock = PitchLock::tracking;

    juce::Random rng;
    juce::uint32 seed = 1;
};
