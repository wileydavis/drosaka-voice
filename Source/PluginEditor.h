#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

class VoiceRow;

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
    juce::Rectangle<int> waveArea() const;

    DrosakaVoiceProcessor& proc;

    juce::TextButton addButton { "Add stems..." };
    juce::TextButton sortPitch { "Pitch" }, sortFormant { "Formant" };
    juce::Slider countKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> countAttachment;
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
