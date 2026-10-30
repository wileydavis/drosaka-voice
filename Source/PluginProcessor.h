#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include "Bank.h"

//==============================================================================
// Milestone 2: voice bank.
//   - a bank of time-aligned stems, all fired together by any MIDI note
//   - auto-sorted by pitch or formant; one primary in the centre
//   - Count builds the crowd outward from the primary in pairs, crossfading smoothly
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

    Bank::Ptr getBank() const;
    juce::StringArray getMissingPaths() const { const juce::ScopedLock l (pathLock); return missingPaths; }

    // 0..1 playhead of the primary for the UI, or -1 when silent.
    float getPlayheadProportion() const { return playhead.load(); }
    // Current (smoothed) crowd gain per stem index, for the UI's level bars.
    float getVoiceLevel (int stemIndex) const
    {
        return stemIndex >= 0 && stemIndex < maxVoices ? voiceLevel[stemIndex].load() : 0.0f;
    }

    // Bumped whenever the bank changes, so the editor rebuilds.
    std::atomic<int> bankGeneration { 0 };

private:
    struct Instance   // one triggered playback of the whole bank
    {
        bool active = false;
        double t = 0.0;          // host samples since trigger
        float env = 0.0f, envTarget = 0.0f, envStep = 0.0f;
    };

    Stem::Ptr importStem (const juce::File&, juce::String& error);
    void swapBank (Bank::Ptr);
    void startInstance();
    void renderChunk (juce::AudioBuffer<float>& out, int start, int num, Bank& bank,
                      const float* g0, const float* g1, const float* p0, const float* p1, int blockLen, int blockOffset);

    juce::AudioFormatManager formatManager;

    juce::SpinLock bankLock;          // guards only the pointer
    Bank::Ptr bank;

    juce::CriticalSection pathLock;
    juce::StringArray missingPaths;

    std::atomic<float>* countParam = nullptr;

    Instance instances[2];
    int current = 0;
    double hostRate = 44100.0;
    int fadeSamples = 64;
    std::atomic<float> playhead { -1.0f };
    std::atomic<float> voiceLevel[maxVoices] {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrosakaVoiceProcessor)
};
