#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include "Bank.h"
#include "LpcCodec.h"

//==============================================================================
// Voice bank (M2) + codec (M3).
//   - a bank of time-aligned stems, all fired together by any MIDI note
//   - auto-sorted by pitch or formant; one primary in the centre
//   - Count builds the crowd outward from the primary in pairs, crossfading smoothly
//   - every voice runs through its own LPC codec; dry voices are delayed to match the
//     codec's latency (reported to the host). Encode count moves the audible voices onto
//     the codec one at a time in a fixed scatter order, primary last.
class DrosakaVoiceProcessor : public juce::AudioProcessor
{
public:
    DrosakaVoiceProcessor();
    ~DrosakaVoiceProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "drosaka Voice"; }
    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==========================================================================
    // Automatable knobs.
    juce::AudioProcessorValueTreeState params;
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    //==========================================================================
    // Message-thread bank API (used by the editor).
    static bool isSupportedAudioFile (const juce::String& path);
    // Imports files; returns the number added, errors appended to 'errors'.
    int addStems (const juce::StringArray& paths, juce::StringArray& errors);
    void removeStem (int stemIndex);
    void setPrimary (int stemIndex);
    void setMute (int stemIndex, bool);
    void setSolo (int stemIndex, bool);
    void setSortAxis (SortAxis);

    // Alignment (all message thread).
    void setOffset (int stemIndex, double seconds);   // manual drag
    void resetOffset (int stemIndex);                 // back to the auto-aligned position
    void alignAll();                                  // re-run auto-align against the primary

    Bank::Ptr getBank() const;
    juce::StringArray getMissingPaths() const { const juce::ScopedLock l (pathLock); return missingPaths; }

    // 0..1 playhead of the primary for the UI, or -1 when silent.
    float getPlayheadProportion() const { return playhead.load(); }
    // Current (smoothed) crowd gain per stem index, for the UI's level bars.
    float getVoiceLevel (int stemIndex) const
    {
        return stemIndex >= 0 && stemIndex < maxVoices ? voiceLevel[stemIndex].load() : 0.0f;
    }

    // Current (smoothed) dry->codec amount per stem index, for the UI.
    float getVoiceEncode (int stemIndex) const
    {
        return stemIndex >= 0 && stemIndex < maxVoices ? voiceEncode[stemIndex].load() : 0.0f;
    }

    // Place in the infection order among audible voices (1-based), 0 if not audible.
    int getVoiceEncodeRank (int stemIndex) const
    {
        return stemIndex >= 0 && stemIndex < maxVoices ? voiceRank[stemIndex].load() : 0;
    }
    std::atomic<int> audibleVoices { 0 };

    // Bumped whenever the bank changes. structureGeneration only when rows must be
    // rebuilt (not for offset drags, which would otherwise kill the drag mid-gesture).
    std::atomic<int> bankGeneration { 0 };
    std::atomic<int> structureGeneration { 0 };

private:
    static constexpr int chunkSize = 512;   // processing sub-block

    struct VoiceDsp   // per bank slot, audio thread only
    {
        LpcCodec codec;
        std::vector<float> delay;   // dry path, delayed by the codec latency
        int delayPos = 0;
        float encode = 0.0f;        // smoothed 0 = dry .. 1 = codec
    };

    struct Instance   // one triggered playback of the whole bank
    {
        bool active = false;
        double t = 0.0;          // host samples since trigger
        float env = 0.0f, envTarget = 0.0f, envStep = 0.0f;
    };

    Stem::Ptr importStem (const juce::File&, juce::String& error);
    void swapBank (Bank::Ptr, bool structural = true);
    void autoAlign (Bank& b, int stemIndex);
    void startInstance();
    void renderDry (int start, int num, Bank& bank);   // all stems, mono, into dryBuf

    juce::AudioFormatManager formatManager;

    juce::SpinLock bankLock;          // guards only the pointer
    Bank::Ptr bank;

    juce::CriticalSection pathLock;
    juce::StringArray missingPaths;

    std::atomic<float>* countParam = nullptr;
    std::atomic<float>* encodeParam = nullptr;
    std::atomic<float>* bitRateParam = nullptr;
    std::atomic<float>* frameDropParam = nullptr;
    std::atomic<float>* pitchLockParam = nullptr;

    VoiceDsp voiceDsp[maxVoices];
    juce::AudioBuffer<float> dryBuf { maxVoices, chunkSize };
    std::vector<float> codecOut = std::vector<float> (chunkSize);
    int latency = 0;

    Instance instances[2];
    int current = 0;
    double hostRate = 44100.0;
    int fadeSamples = 64;
    std::atomic<float> playhead { -1.0f };
    std::atomic<float> voiceLevel[maxVoices] {};
    std::atomic<float> voiceEncode[maxVoices] {};
    std::atomic<int> voiceRank[maxVoices] {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrosakaVoiceProcessor)
};
