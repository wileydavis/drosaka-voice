#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <vector>
#include <algorithm>
#include "Analysis.h"
#include "Align.h"

constexpr int maxVoices = 32;   // spec designs for up to 20

enum class SortAxis { pitch = 0, formant = 1 };

//==============================================================================
// One decoded stem (mono). Audio data is immutable after import; the two
// "cur" fields are touched only by the audio thread (smoothed crowd state).
struct Stem : public juce::ReferenceCountedObject
{
    using Ptr = juce::ReferenceCountedObjectPtr<Stem>;

    juce::AudioBuffer<float> audio;   // 1 channel, at the file's own rate
    double sampleRate = 44100.0;
    juce::File file;
    std::vector<float> peaks;         // min/max pairs for drawing
    StemAnalysis analysis;
    std::vector<float> alignEnv;      // syllable-rhythm envelope for auto-alignment
    PitchMarks marks;                 // for PSOLA pitch shifting (Spread)
    juce::MemoryBlock flac;           // the audio, losslessly compressed, for embedding in the set

    float curGain = 0.0f;             // audio thread only
    float curPan = 0.0f;              // audio thread only
};

enum class AlignState { reference = 0, autoAligned = 1, unsure = 2, manual = 3 };

//==============================================================================
// Slice markers, resolved for use (seconds on the phrase timeline, in order).
// Each marker k has a home position ('base', set on the waveform, saved with the set) plus
// an automatable offset (parameter "<note> slice offset", 0 in the middle of the lane,
// +-500 ms). Marker k always starts the slice on key C3 + k + 1, so each offset parameter
// always means one note. N markers make N+1 regions; region k plays on C3 + k.
struct SliceSet
{
    static constexpr int maxMarkers = 15;
    static constexpr float maxOffsetMs = 500.0f;
    double t[maxMarkers] {};      // effective time (base + offset, kept in order)
    double base[maxMarkers] {};   // home position
    int slot[maxMarkers] {};      // which marker/parameter (0 = the C#3 one)
    int n = 0;

    int numRegions() const { return n > 0 ? n + 1 : 0; }

    // A slice key's region, or the whole phrase for any other key.
    void region (int note, double lengthSec, double& startSec, double& endSec) const
    {
        startSec = 0.0;
        endSec = lengthSec;
        const int k = note - 60;
        if (n == 0 || k < 0 || k > n) return;
        if (k > 0) startSec = t[k - 1];
        if (k < n) endSec = t[k];
    }

    // bases: seconds, packed and ascending, 0 = unused. offsetsMs: per marker.
    // A marker offset past its neighbour stops just after it (never overtakes).
    static SliceSet resolve (const float* bases, const float* offsetsMs, double lengthSec)
    {
        SliceSet s;
        for (int i = 0; i < maxMarkers; ++i)
        {
            if (bases[i] <= 0.0f) break;
            double t = juce::jlimit (0.003, juce::jmax (0.003, lengthSec - 0.003), (double) bases[i] + offsetsMs[i] * 0.001);
            if (s.n > 0) t = std::max (t, s.t[s.n - 1] + 0.008);
            if (t >= lengthSec - 0.002) break;
            s.t[s.n] = t; s.base[s.n] = bases[i]; s.slot[s.n] = i; ++s.n;
        }
        return s;
    }
};

struct VoiceFlags
{
    bool mute = false, solo = false;
    double offsetSec = 0.0;       // applied time shift (+ = later)
    double autoOffsetSec = 0.0;   // what auto-align found (double-click resets to it)
    AlignState align = AlignState::reference;
};

//==============================================================================
// Immutable snapshot of the bank. Any edit builds a new one and swaps it in.
struct Bank : public juce::ReferenceCountedObject
{
    using Ptr = juce::ReferenceCountedObjectPtr<Bank>;

    std::vector<Stem::Ptr> stems;     // import order
    std::vector<VoiceFlags> flags;    // parallel to stems
    int primary = -1;                 // index into stems
    SortAxis axis = SortAxis::pitch;
    std::vector<int> sorted;          // stem indices, low -> high on 'axis'
    std::vector<int> encodeOrder;     // stem indices in the order the codec takes them; primary last

    int size() const { return (int) stems.size(); }

    // Length of the phrase timeline (seconds): the latest end of any stem, with alignment.
    double lengthSeconds() const
    {
        double longest = 0.0;
        for (size_t i = 0; i < stems.size(); ++i)
            longest = std::max (longest, stems[i]->audio.getNumSamples() / stems[i]->sampleRate + std::max (0.0, flags[i].offsetSec));
        return longest;
    }

    static constexpr int sliceRoot = 60;   // C3 in Live's naming: region k plays on sliceRoot + k

    void resort()
    {
        sorted.resize (stems.size());
        for (size_t i = 0; i < sorted.size(); ++i) sorted[i] = (int) i;
        auto key = [this] (int i)
        {
            const auto& a = stems[(size_t) i]->analysis;
            return axis == SortAxis::pitch ? std::make_pair (a.pitchHz, a.centroidHz)
                                           : std::make_pair (a.centroidHz, a.pitchHz);
        };
        std::stable_sort (sorted.begin(), sorted.end(), [&] (int a, int b) { return key (a) < key (b); });
        if (primary < 0 && ! stems.empty()) primary = 0;
        if (primary >= size()) primary = size() - 1;
        computeEncodeOrder();
    }

    // Fixed-seed scatter: alternate left/right of the primary (in the sorted array,
    // which is also how the crowd is panned), random within each side, so the codec
    // jumps around the head. The primary always falls last.
    void computeEncodeOrder()
    {
        encodeOrder.clear();
        const int n = size();
        if (n == 0) return;
        int p = 0;
        for (int s = 0; s < n; ++s) if (sorted[(size_t) s] == primary) { p = s; break; }

        std::vector<int> left, right;
        for (int s = 0; s < n; ++s)
            if (s < p) left.push_back (sorted[(size_t) s]);
            else if (s > p) right.push_back (sorted[(size_t) s]);

        juce::Random rng (0x0D705A4A);   // fixed: the same bank always infects in the same order
        auto shuffle = [&] (std::vector<int>& v)
        {
            for (int i = (int) v.size() - 1; i > 0; --i) std::swap (v[(size_t) i], v[(size_t) rng.nextInt (i + 1)]);
        };
        shuffle (left);
        shuffle (right);

        bool fromLeft = rng.nextBool();
        size_t li = 0, ri = 0;
        while (li < left.size() || ri < right.size())
        {
            if ((fromLeft && li < left.size()) || ri >= right.size()) encodeOrder.push_back (left[li++]);
            else encodeOrder.push_back (right[ri++]);
            fromLeft = ! fromLeft;
        }
        encodeOrder.push_back (primary);
    }

    Ptr clone() const
    {
        Ptr b = new Bank();
        b->stems = stems; b->flags = flags; b->primary = primary; b->axis = axis; b->sorted = sorted;
        b->encodeOrder = encodeOrder;
        return b;
    }
};

//==============================================================================
// The crowd: target gain and pan for each stem (indexed like bank.stems),
// for Count = countVoices (0 .. bank size, continuous). Pure, allocation-free.
inline void computeCrowd (const Bank& bank, float countVoices, float* gain, float* pan)
{
    const int n = juce::jmin (bank.size(), maxVoices);
    for (int i = 0; i < n; ++i) { gain[i] = 0.0f; pan[i] = 0.0f; }
    if (n == 0) return;

    // Primary's position in the sorted array.
    int p = 0;
    for (int s = 0; s < n; ++s) if (bank.sorted[(size_t) s] == bank.primary) { p = s; break; }

    // Rank sorted positions by distance from the primary: p, p-1, p+1, p-2, p+2...
    // (at an edge the window keeps growing on the side that has voices).
    int rankPos[maxVoices];
    int r = 0;
    rankPos[r++] = p;
    for (int k = 1; r < n; ++k)
    {
        if (p - k >= 0) rankPos[r++] = p - k;
        if (p + k < n && r < n) rankPos[r++] = p + k;
    }

    const int numPairs = n / 2;   // pair index of the last rank
    for (r = 0; r < n; ++r)
    {
        const int stemIdx = bank.sorted[(size_t) rankPos[r]];
        if (r == 0)
        {
            gain[stemIdx] = juce::jlimit (0.0f, 1.0f, countVoices);
            pan[stemIdx] = 0.0f;
            continue;
        }
        const int k = (r + 1) / 2;
        const float lo = (float) (2 * k - 1);
        const float hi = (float) juce::jmin (2 * k + 1, n);
        gain[stemIdx] = juce::jlimit (0.0f, 1.0f, (countVoices - lo) / (hi - lo));

        // Pan: wider with distance; lower-sorted member of a pair to the left.
        const float width = numPairs > 0 ? (float) k / (float) numPairs : 0.0f;
        const int partnerRank = (r % 2 == 1) ? r + 1 : r - 1;
        bool left;
        if (partnerRank < n && partnerRank >= 1 && (partnerRank + 1) / 2 == k)
            left = rankPos[r] < rankPos[partnerRank];
        else
            left = rankPos[r] < p;
        pan[stemIdx] = left ? -width : width;
    }

    // Solo auditions regardless of Count; mute silences in place.
    bool anySolo = false;
    for (int i = 0; i < n; ++i) anySolo |= bank.flags[(size_t) i].solo;
    for (int i = 0; i < n; ++i)
    {
        if (anySolo) gain[i] = bank.flags[(size_t) i].solo ? 1.0f : 0.0f;
        if (bank.flags[(size_t) i].mute && ! bank.flags[(size_t) i].solo) gain[i] = 0.0f;
    }

    // Keep loudness roughly even as the crowd grows.
    float power = 0.0f;
    for (int i = 0; i < n; ++i) power += gain[i] * gain[i];
    const float norm = 1.0f / std::sqrt (juce::jmax (1.0f, power));
    for (int i = 0; i < n; ++i) gain[i] *= norm;
}

//==============================================================================
// Encode count over the AUDIBLE voices only (crowdGain > 0: inside the Count window,
// not muted, or soloed). encodeFraction 0..1 maps to 0..A audible voices, taken one
// at a time in encodeOrder (primary last). rank[] gets each voice's place among the
// audible ones (1-based, 0 = not audible), for the UI.
inline void computeEncode (const Bank& bank, float encodeFraction, const float* crowdGain, float* encode, int* rank = nullptr)
{
    const int n = juce::jmin (bank.size(), maxVoices);
    int audible = 0;
    for (int i = 0; i < n; ++i) audible += crowdGain[i] > 0.0f ? 1 : 0;
    const float e = juce::jlimit (0.0f, 1.0f, encodeFraction) * (float) audible;

    // Silent voices wait dry, unless everything is encoded (then they arrive encoded).
    const float idle = encodeFraction >= 0.999f ? 1.0f : 0.0f;
    for (int i = 0; i < n; ++i) { encode[i] = idle; if (rank != nullptr) rank[i] = 0; }

    int r = 0;
    for (int idx : bank.encodeOrder)
    {
        if (idx >= n || crowdGain[idx] <= 0.0f) continue;
        encode[idx] = juce::jlimit (0.0f, 1.0f, e - (float) r);
        ++r;
        if (rank != nullptr) rank[idx] = r;
    }
}
