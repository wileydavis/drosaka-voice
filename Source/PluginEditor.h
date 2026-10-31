#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

class VoiceRow;
class SliceView;
class KnobCell;

// Shared time axis for the alignment lanes (seconds). Frozen while a lane is dragged.
struct Timeline { double start = 0.0, end = 1.0; bool frozen = false; };

// Layout: header / slice editor (full width) / 4x4 control grid + performance panel / voices.
class DrosakaVoiceEditor : public juce::AudioProcessorEditor,
                           public juce::FileDragAndDropTarget,
                           private juce::Timer
{
public:
    explicit DrosakaVoiceEditor (DrosakaVoiceProcessor&);
    ~DrosakaVoiceEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dragHover = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { dragHover = false; repaint(); }
    void filesDropped (const juce::StringArray& files, int, int) override;

private:
    void timerCallback() override;
    void addFiles (const juce::StringArray&);
    void rebuildRows();
    void updateTimeline();
    juce::Rectangle<int> waveArea() const;
    juce::Rectangle<int> gridArea() const;
    juce::Rectangle<int> panelArea() const;
    juce::Rectangle<int> fieldArea() const;

    DrosakaVoiceProcessor& proc;

    juce::TextButton addButton { "Add stems..." };
    juce::TextButton alignButton { "Align all" };
    juce::TextButton presetButton { "Preset" };
    juce::TextButton sortPitch { "Pitch" }, sortFormant { "Formant" };
    std::unique_ptr<SliceView> sliceView;
    std::vector<std::unique_ptr<KnobCell>> cells;   // the 4x4 grid

    juce::ComboBox playModeBox, pitchLockBox, quantBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> playModeAttachment, pitchLockAttachment, quantAttachment;
    juce::TextButton dropButton { "DROP" }, keepHumanButton { "KEEP HUMAN" }, latchButton { "LATCH" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> dropAttachment, keepHumanAttachment, latchAttachment;

    juce::Viewport listView;
    juce::Component listContent;
    std::vector<std::unique_ptr<VoiceRow>> rows;
    std::unique_ptr<juce::FileChooser> chooser;

    Timeline timeline;
    bool timelineDirty = false;
    int seenStructure = -1;
    Bank::Ptr shownBank;
    int seenGeneration = -1;
    float lastPlayhead = -1.0f;
    bool dragHover = false;
    juce::String status;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrosakaVoiceEditor)
};
