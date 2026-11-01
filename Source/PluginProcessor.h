#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <limits>
#include "Bank.h"
#include "LpcCodec.h"
#include "Spread.h"
#include "Spatial.h"
#include "Pattern.h"

//==============================================================================
// Voice bank (M2) + codec (M3).
//   - a bank of time-aligned stems, all fired together by any MIDI note
//   - auto-sorted by pitch or formant; one primary in the centre
//   - Count builds the crowd outward from the primary in pairs, crossfading smoothly
//   - every voice runs through its own LPC codec; dry voices are delayed to match the
//     codec's latency (reported to the host). Encode count moves the audible voices onto
//     the codec one at a time in a fixed scatter order, primary last.
class DrosakaVoiceProcessor : public juce::AudioProcessor,
                              private juce::Timer
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

    // Slicing (message thread). Markers are seconds on the phrase timeline.
    // Markers: a home position each (edited here, saved with the set) plus an automatable
    // offset parameter per marker ("C#3 slice offset" ...), 0 = the middle of the lane.
    void addSlice (double seconds);
    void moveSlice (int index, double seconds);       // index in time order; clamped between neighbours
    void removeSlice (int index);
    SliceSet getSlices() const;                       // resolved for the current bank

    // MIDI learn: any parameter can be driven by a MIDI CC (any channel). The editor
    // arms a parameter; the next CC that arrives is bound to it. Mappings save with the set.
    void armMidiLearn (const juce::String& paramId);
    void cancelMidiLearn() { learnParam = -1; }
    juce::String getLearningParam() const;
    int getCcFor (const juce::String& paramId) const;      // -1 if none
    void forgetCc (const juce::String& paramId);
    void applyPendingMidi();                                // message thread: push CC moves to the host
    std::atomic<int> midiMapGeneration { 0 };

    // Recording: route a track into the device's Sidechain input, then turn on "record".
    // When it turns off, the take is trimmed of silence, saved as a WAV in the takes folder
    // and added to the bank (auto-aligned to the primary).
    static constexpr double maxTakeSeconds = 60.0;
    void processFinishedTake();                       // message thread (timer); public for tests
    void setTakesFolder (const juce::File& f) { const juce::ScopedLock l (pathLock); takesFolder = f; }
    juce::File getTakesFolder() const { const juce::ScopedLock l (pathLock); return takesFolder; }
    juce::String getRecordMessage() const { const juce::ScopedLock l (pathLock); return recordMessage; }
    std::atomic<bool> recordingNow { false };
    std::atomic<float> recordSeconds { 0.0f }, recordLevel { 0.0f };
    std::atomic<float> inputLevel { 0.0f };    // input meter, live even when not recording
    std::atomic<int> inputChannels { 0 };      // how many input channels the host is giving us

    // Presets: the same state the Live set stores (bank with embedded audio, slices, knobs).
    bool savePreset (const juce::File&);
    bool loadPreset (const juce::File&);

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
    std::atomic<float> hostBpm { 120.0f };
    std::atomic<bool> patternActive { false };
    std::atomic<int> patternSlice { -1 };
    std::atomic<bool> hostPlaying { false };
    float getVoicePan (int stemIndex) const
    {
        return stemIndex >= 0 && stemIndex < maxVoices ? voicePan[stemIndex].load() : 0.0f;
    }

    // Current pitch offset per stem (semitones, as heard: dry/codec blend), for the UI.
    float getVoiceSemis (int stemIndex) const
    {
        return stemIndex >= 0 && stemIndex < maxVoices ? voiceSemis[stemIndex].load() : 0.0f;
    }

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
        float drySemis = 0.0f, codecSemis = 0.0f;   // smoothed Spread intervals
        float psolaMix = 0.0f;      // 0 = direct playback .. 1 = pitch-shifted (PSOLA)
        std::vector<float> depthRing;   // depth pre-delay
        int depthPos = 0;
        float depth = 0.0f;         // smoothed depth amount for this voice
    };

    struct Grain { double synthCenter, srcCenter, half; float gain; };
    struct Psola
    {
        double nextSynth = 0.0;     // host samples since trigger
        Grain grains[8];
        int count = 0;
    };

    struct Instance   // one triggered playback of the whole bank
    {
        bool active = false;
        double startPos = 0.0;   // where on the phrase timeline the hit starts (host samples)
        double out = 0.0;        // output samples since the hit
        double speed = 1.0;      // phrase samples per output sample (pattern stretch)
        double stopOut = 1e18;   // hit length in output samples (slice end, or step x gate)
        float pitchSemis = 0.0f; // per-hit transpose (Screwed)
        bool releasing = false;
        float env = 0.0f, envTarget = 0.0f, envStep = 0.0f;
        Psola psola[maxVoices];      // Spread (x hit pitch): shiftBuf
        Psola psolaRaw[maxVoices];   // hit pitch only, for stretched/pitched hits: dryBuf
    };

    Stem::Ptr importStem (const juce::File&, juce::String& error);
    Stem::Ptr readStem (juce::AudioFormatReader*, const juce::File& displayFile, juce::String& error, bool encodeForEmbedding);
    void swapBank (Bank::Ptr, bool structural = true);
    void autoAlign (Bank& b, int stemIndex);
    void startInstance (double startPos, double lengthOut, double speed, float pitchSemis);
    // All stems, mono: direct playback into dryBuf, pitch-shifted into shiftBuf where asked.
    void renderDry (int start, int num, Bank& bank, const float* psolaRatio, const bool* wantPsola);

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
    juce::AudioBuffer<float> shiftBuf { maxVoices, chunkSize };
    float envBuf[chunkSize] {};
    std::atomic<float>* spreadParam = nullptr;
    std::atomic<float>* chordParam = nullptr;
    std::atomic<float>* snapParam = nullptr;
    std::atomic<float> voiceSemis[maxVoices] {};
    std::atomic<float> voicePan[maxVoices] {};
    std::atomic<float>* spatialParam = nullptr;
    std::atomic<float>* motionDivParam = nullptr;
    std::atomic<float>* encodeQuantParam = nullptr;
    double internalPpq = 0.0, lastBpm = 120.0;

    // Pattern mode (audio thread).
    struct PendingHit { double ppq; pattern::Hit hit; };
    std::atomic<float>* playModeParam = nullptr;
    std::atomic<float>* patternTypeParam = nullptr;
    std::atomic<float>* patternDivParam = nullptr;
    std::atomic<float>* gateParam = nullptr;
    std::atomic<float>* densityParam = nullptr;
    std::atomic<float>* repeatsParam = nullptr;
    std::atomic<float>* dropSemisParam = nullptr;
    std::atomic<float>* latchParam = nullptr;
    std::atomic<float>* dropParam = nullptr;
    std::atomic<float>* keepHumanParam = nullptr;
    std::atomic<float>* depthParam = nullptr;
    juce::Reverb reverb;
    float sendL[chunkSize] {}, sendR[chunkSize] {};
    bool patternRunning = false;
    int lastPatternType = -1;
    pattern::State patternState;
    double nextStepPpq = 0.0;
    PendingHit pending[32];
    int numPending = 0;
    bool held[128] {};
    void patternNote (const juce::MidiMessage&, double ppqAtEvent, const Bank&);
    SliceSet liveSlices;   // audio thread: the markers as of this block
    tune::Spec liveTune;   // audio thread: autotune as of this block
    std::atomic<float>* tuneAmountParam = nullptr;
    std::atomic<float>* tuneRootParam = nullptr;
    std::atomic<float>* tuneScaleParam = nullptr;
    std::atomic<float> sliceBase[SliceSet::maxMarkers] {};        // seconds, packed ascending, 0 = unused
    std::atomic<float>* sliceOffsetParams[SliceSet::maxMarkers] {};
    void setBases (const std::vector<double>& bases);
    void setOffsetPct (int slot, float pct);
    std::vector<double> currentBases() const;
    void fireHit (const pattern::Hit&, const Bank&, double bpm);
    juce::int64 lastEncodeStep = std::numeric_limits<juce::int64>::min();
    float latchedEncodeVoices = 0.0f;
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

    // MIDI learn state: CC number -> parameter index (into getParameters()).
    std::atomic<int> ccToParam[128];
    std::atomic<int> learnParam { -1 };
    std::atomic<float> pendingCc[128];        // latest value per CC, -1 = nothing new
    void timerCallback() override { applyPendingMidi(); processFinishedTake(); }

    // Take recording (two buffers, so a new take can start while the last is being saved).
    std::vector<float> takeBuf[2];
    int takeIndex = 0, takePos = 0;
    bool takeRecording = false, takeFull = false;
    std::atomic<int> finishedTake { -1 };       // buffer index ready for the message thread
    std::atomic<int> finishedLength { 0 };
    std::atomic<float> finishedPeak { 0.0f };
    std::atomic<int> finishedChannels { 0 };
    float takePeak = 0.0f;
    int takeChannels = 0;
    std::atomic<bool> stopRequested { false };  // buffer filled: turn the record switch off
    std::atomic<float>* recordParam = nullptr;
    juce::File takesFolder;
    juce::String recordMessage;
    int takeCounter = 0;
    void captureTake (const juce::AudioBuffer<float>& buffer);
    void handleCc (const juce::MidiMessage&);
    int paramIndex (const juce::String& id) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrosakaVoiceProcessor)
};
