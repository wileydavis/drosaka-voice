#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>

//==============================================================================
// One decoded stem. Immutable after loading, shared between threads by
// reference count so the audio thread never touches a freed buffer.
struct Stem : public juce::ReferenceCountedObject
{
    using Ptr = juce::ReferenceCountedObjectPtr<Stem>;

    juce::AudioBuffer<float> audio;   // 1 or 2 channels, at the file's own rate
    double sampleRate = 44100.0;
    juce::File file;
    std::vector<float> peaks;         // for drawing the waveform (min/max pairs)
};

//==============================================================================
// Milestone 1: sample player.
//   - holds one stem
//   - any MIDI note-on plays it from the start (pitch is trigger only)
//   - plays to the end (one-shot); a new note restarts it with a short crossfade
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
    // Message-thread API used by the editor.
    bool loadStem (const juce::File& file, juce::String& errorOut);
    Stem::Ptr getStem() const;
    static bool isSupportedAudioFile (const juce::String& path);

    // 0..1 playhead for the UI, or -1 when silent.
    float getPlayheadProportion() const { return playhead.load(); }

    // Bumped whenever a new stem is installed, so the editor knows to redraw.
    std::atomic<int> stemGeneration { 0 };

    // If a saved set points at a file that has moved, keep the path (so saving
    // doesn't erase it) and let the UI say what's missing.
    juce::String getMissingPath() const { const juce::ScopedLock l (pathLock); return missingPath; }

private:
    struct Voice
    {
        bool active = false;
        double pos = 0.0;      // in source samples
        float gain = 0.0f;
        float gainTarget = 0.0f;
        float gainStep = 0.0f;
    };

    void startVoice();
    void renderVoices (juce::AudioBuffer<float>& out, int start, int num, const Stem& stem);

    juce::AudioFormatManager formatManager;

    juce::SpinLock stemLock;   // guards only the pointer swap
    Stem::Ptr stem;

    juce::CriticalSection pathLock;
    juce::String missingPath;

    // Two slots: the current voice and one fading out after a retrigger.
    Voice voices[2];
    int current = 0;

    double hostRate = 44100.0;
    int fadeSamples = 64;
    std::atomic<float> playhead { -1.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrosakaVoiceProcessor)
};
