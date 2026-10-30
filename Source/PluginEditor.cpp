#include "PluginEditor.h"

namespace colours
{
    const juce::Colour bg     { 0xff111214 };
    const juce::Colour panel  { 0xff1b1d21 };
    const juce::Colour wave   { 0xffd8d4c8 };
    const juce::Colour accent { 0xffff5a1f };
    const juce::Colour dim    { 0xff6b6f78 };
}

DrosakaVoiceEditor::DrosakaVoiceEditor (DrosakaVoiceProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    addAndMakeVisible (loadButton);
    loadButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Load a voice stem", juce::File(),
                                                       "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.m4a");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
                              {
                                  if (fc.getResult().existsAsFile()) load (fc.getResult());
                              });
    };

    setSize (600, 240);
    startTimerHz (30);
}

void DrosakaVoiceEditor::load (const juce::File& f)
{
    juce::String err;
    status = proc.loadStem (f, err) ? juce::String() : err;
    timerCallback();
    repaint();
}

bool DrosakaVoiceEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (DrosakaVoiceProcessor::isSupportedAudioFile (f)) return true;
    return false;
}

void DrosakaVoiceEditor::filesDropped (const juce::StringArray& files, int, int)
{
    dragHover = false;
    // Milestone 1 holds one stem: take the first audio file dropped.
    for (auto& f : files)
        if (DrosakaVoiceProcessor::isSupportedAudioFile (f)) { load (juce::File (f)); return; }
    repaint();
}

void DrosakaVoiceEditor::timerCallback()
{
    const int gen = proc.stemGeneration.load();
    if (gen != seenGeneration)
    {
        seenGeneration = gen;
        shownStem = proc.getStem();
        repaint();
    }

    const float ph = proc.getPlayheadProportion();
    if (! juce::exactlyEqual (ph, lastPlayhead))
    {
        lastPlayhead = ph;
        repaint (waveArea());
    }
}

juce::Rectangle<int> DrosakaVoiceEditor::waveArea() const
{
    return getLocalBounds().reduced (16).withTrimmedTop (44).withTrimmedBottom (22);
}

void DrosakaVoiceEditor::resized()
{
    loadButton.setBounds (getWidth() - 16 - 120, 14, 120, 26);
}

void DrosakaVoiceEditor::paint (juce::Graphics& g)
{
    g.fillAll (colours::bg);

    g.setColour (colours::wave);
    g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    g.drawText ("drosaka voice", 16, 14, 300, 26, juce::Justification::centredLeft);

    auto area = waveArea();
    g.setColour (colours::panel);
    g.fillRoundedRectangle (area.toFloat(), 6.0f);
    if (dragHover)
    {
        g.setColour (colours::accent);
        g.drawRoundedRectangle (area.toFloat().reduced (1.0f), 6.0f, 2.0f);
    }

    auto footer = getLocalBounds().reduced (16, 0).removeFromBottom (24);
    g.setFont (juce::FontOptions (13.0f));

    if (shownStem == nullptr)
    {
        g.setColour (colours::dim);
        const auto missing = proc.getMissingPath();
        g.drawText (missing.isNotEmpty() ? "Missing: " + missing + "  (drop it again)"
                                         : "Drop a voice stem here",
                    area, juce::Justification::centred);
        if (status.isNotEmpty()) { g.setColour (colours::accent); g.drawText (status, footer, juce::Justification::centredLeft); }
        return;
    }

    // Waveform from precomputed min/max peaks.
    const auto& peaks = shownStem->peaks;
    const int bins = (int) peaks.size() / 2;
    const float midY = (float) area.getCentreY();
    const float halfH = area.getHeight() * 0.45f;
    g.setColour (colours::wave);
    for (int x = 0; x < area.getWidth(); ++x)
    {
        const int b = juce::jlimit (0, bins - 1, x * bins / area.getWidth());
        const float lo = peaks[(size_t) b * 2], hi = peaks[(size_t) b * 2 + 1];
        g.drawVerticalLine (area.getX() + x, midY - hi * halfH, midY - lo * halfH + 1.0f);
    }

    if (lastPlayhead >= 0.0f)
    {
        g.setColour (colours::accent);
        const int px = area.getX() + (int) (lastPlayhead * area.getWidth());
        g.fillRect (px, area.getY(), 2, area.getHeight());
    }

    const double secs = shownStem->audio.getNumSamples() / shownStem->sampleRate;
    g.setColour (status.isNotEmpty() ? colours::accent : colours::dim);
    g.drawText (status.isNotEmpty() ? status
                                    : shownStem->file.getFileName() + "   "
                                      + juce::String (secs, 2) + " s   "
                                      + juce::String (shownStem->sampleRate / 1000.0, 1) + " kHz   "
                                      + (shownStem->audio.getNumChannels() > 1 ? "stereo" : "mono")
                                      + "   any MIDI note plays it",
                footer, juce::Justification::centredLeft);
}
