#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

class DrosakaVoiceEditor : public juce::AudioProcessorEditor,
                           public juce::FileDragAndDropTarget,
                           private juce::Timer
{
public:
    explicit DrosakaVoiceEditor (DrosakaVoiceProcessor&);
    ~DrosakaVoiceEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dragHover = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { dragHover = false; repaint(); }
    void filesDropped (const juce::StringArray& files, int, int) override;

private:
    void timerCallback() override;
    void load (const juce::File&);
    juce::Rectangle<int> waveArea() const;

    DrosakaVoiceProcessor& proc;
    juce::TextButton loadButton { "Load stem..." };
    std::unique_ptr<juce::FileChooser> chooser;

    Stem::Ptr shownStem;
    int seenGeneration = -1;
    float lastPlayhead = -1.0f;
    bool dragHover = false;
    juce::String status;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrosakaVoiceEditor)
};
