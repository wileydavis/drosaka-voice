#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

class VoiceRow;

// Shared time axis for the alignment lanes (seconds). Frozen while a lane is dragged.
struct Timeline { double start = 0.0, end = 1.0; bool frozen = false; };

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

    DrosakaVoiceProcessor& proc;

    juce::TextButton addButton { "Add stems..." };
    juce::TextButton alignButton { "Align all" };
    Timeline timeline;
    bool timelineDirty = false;
    int seenStructure = -1;
    juce::TextButton sortPitch { "Pitch" }, sortFormant { "Formant" };
    juce::Slider countKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> countAttachment;

    // Codec strip.
    juce::Slider encodeKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider bitRateKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider frameDropKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::ComboBox pitchLockBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> encodeAttachment, bitRateAttachment, frameDropAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> pitchLockAttachment;
    juce::Rectangle<int> codecArea() const;

    // Spread strip.
    juce::Slider spreadKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider snapKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::ComboBox chordBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> spreadAttachment, snapAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> chordAttachment;
    juce::Rectangle<int> spreadArea() const;

    // Space & time strip.
    juce::ComboBox spatialBox, motionBox, quantBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> spatialAttachment, motionAttachment, quantAttachment;
    juce::Rectangle<int> spaceArea() const;
    juce::Rectangle<int> fieldArea() const;
    juce::Viewport listView;
    juce::Component listContent;
    std::vector<std::unique_ptr<VoiceRow>> rows;
    std::unique_ptr<juce::FileChooser> chooser;

    Bank::Ptr shownBank;
    int seenGeneration = -1;
    float lastPlayhead = -1.0f;
    bool dragHover = false;
    juce::String status;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrosakaVoiceEditor)
};
