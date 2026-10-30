#include "PluginEditor.h"

namespace colours
{
    const juce::Colour bg     { 0xff111214 };
    const juce::Colour panel  { 0xff1b1d21 };
    const juce::Colour row    { 0xff202328 };
    const juce::Colour wave   { 0xffd8d4c8 };
    const juce::Colour accent { 0xffff5a1f };
    const juce::Colour level  { 0x55ff5a1f };
    const juce::Colour dim    { 0xff6b6f78 };
}

namespace
{
    juce::String formatHz (float hz)
    {
        if (hz <= 0.0f) return "—";
        return hz >= 1000.0f ? juce::String (hz / 1000.0f, 2) + " kHz" : juce::String (juce::roundToInt (hz)) + " Hz";
    }

    void styleToggle (juce::TextButton& b, juce::Colour on)
    {
        b.setClickingTogglesState (false);
        b.setColour (juce::TextButton::buttonColourId, colours::row);
        b.setColour (juce::TextButton::buttonOnColourId, on);
        b.setColour (juce::TextButton::textColourOffId, colours::dim);
        b.setColour (juce::TextButton::textColourOnId, colours::bg);
    }
}

//==============================================================================
// One stem in the sorted list: level bar, name, analysis, P / M / S / x.
class VoiceRow : public juce::Component
{
public:
    VoiceRow (DrosakaVoiceProcessor& p, const Bank& b, int stemIdx) : proc (p), idx (stemIdx)
    {
        const auto& s = *b.stems[(size_t) idx];
        name = s.file.getFileNameWithoutExtension();
        info = formatHz (s.analysis.pitchHz) + "   " + formatHz (s.analysis.centroidHz);
        isPrimary = b.primary == idx;

        for (auto* btn : { &primaryBtn, &muteBtn, &soloBtn, &removeBtn }) { addAndMakeVisible (*btn); styleToggle (*btn, colours::accent); }
        styleToggle (muteBtn, colours::dim);
        styleToggle (soloBtn, juce::Colour (0xffe8c547));
        primaryBtn.setToggleState (isPrimary, juce::dontSendNotification);
        muteBtn.setToggleState (b.flags[(size_t) idx].mute, juce::dontSendNotification);
        soloBtn.setToggleState (b.flags[(size_t) idx].solo, juce::dontSendNotification);

        primaryBtn.setTooltip ("Primary voice (centre)");
        muteBtn.setTooltip ("Mute");
        soloBtn.setTooltip ("Solo (auditions regardless of Count)");
        removeBtn.setTooltip ("Remove from bank");

        primaryBtn.onClick = [this] { proc.setPrimary (idx); };
        muteBtn.onClick    = [this] { proc.setMute (idx, ! muteBtn.getToggleState()); };
        soloBtn.onClick    = [this] { proc.setSolo (idx, ! soloBtn.getToggleState()); };
        removeBtn.onClick  = [this] { proc.removeStem (idx); };
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (2);
        removeBtn.setBounds (r.removeFromRight (22)); r.removeFromRight (4);
        soloBtn.setBounds (r.removeFromRight (22));   r.removeFromRight (2);
        muteBtn.setBounds (r.removeFromRight (22));   r.removeFromRight (2);
        primaryBtn.setBounds (r.removeFromRight (22));
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().reduced (0, 1);
        g.setColour (colours::row);
        g.fillRect (r);

        const float lvl = proc.getVoiceLevel (idx);
        const int textW = r.getWidth() - 4 * 26 - 8;
        g.setColour (colours::level);
        g.fillRect (r.withWidth (juce::roundToInt (lvl * (float) textW)));
        if (isPrimary) { g.setColour (colours::accent); g.fillRect (r.withWidth (3)); }

        g.setFont (juce::FontOptions (13.0f));
        g.setColour (colours::wave);
        g.drawText (name, r.withWidth (textW).withTrimmedLeft (10), juce::Justification::centredLeft, true);
        g.setColour (colours::dim);
        g.drawText (info, r.withWidth (textW - 8), juce::Justification::centredRight);
    }

private:
    DrosakaVoiceProcessor& proc;
    int idx;
    juce::String name, info;
    bool isPrimary = false;
    juce::TextButton primaryBtn { "P" }, muteBtn { "M" }, soloBtn { "S" }, removeBtn { "x" };
};

//==============================================================================
DrosakaVoiceEditor::DrosakaVoiceEditor (DrosakaVoiceProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    addAndMakeVisible (addButton);
    addButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Add voice stems", juce::File(),
                                                       "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.m4a");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectMultipleItems,
                              [this] (const juce::FileChooser& fc)
                              {
                                  juce::StringArray paths;
                                  for (auto& f : fc.getResults()) paths.add (f.getFullPathName());
                                  addFiles (paths);
                              });
    };

    for (auto* b : { &sortPitch, &sortFormant }) { addAndMakeVisible (*b); styleToggle (*b, colours::wave); }
    sortPitch.onClick   = [this] { proc.setSortAxis (SortAxis::pitch); };
    sortFormant.onClick = [this] { proc.setSortAxis (SortAxis::formant); };

    countKnob.setColour (juce::Slider::rotarySliderFillColourId, colours::accent);
    countKnob.setColour (juce::Slider::rotarySliderOutlineColourId, colours::row);
    countKnob.setColour (juce::Slider::thumbColourId, colours::wave);
    addAndMakeVisible (countKnob);
    countAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.params, "count", countKnob);
    countKnob.onValueChange = [this] { repaint(); };

    listView.setViewedComponent (&listContent, false);
    listView.setScrollBarsShown (true, false);
    addAndMakeVisible (listView);

    setSize (760, 520);
    startTimerHz (30);
}

DrosakaVoiceEditor::~DrosakaVoiceEditor()
{
    rows.clear();
}

void DrosakaVoiceEditor::addFiles (const juce::StringArray& paths)
{
    juce::StringArray errors;
    proc.addStems (paths, errors);
    status = errors.joinIntoString ("   ");
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
    addFiles (files);
}

void DrosakaVoiceEditor::rebuildRows()
{
    rows.clear();
    if (shownBank == nullptr) return;
    // Low at the top, high at the bottom, in sorted order.
    for (int s : shownBank->sorted)
    {
        rows.push_back (std::make_unique<VoiceRow> (proc, *shownBank, s));
        listContent.addAndMakeVisible (*rows.back());
    }
    sortPitch.setToggleState (shownBank->axis == SortAxis::pitch, juce::dontSendNotification);
    sortFormant.setToggleState (shownBank->axis == SortAxis::formant, juce::dontSendNotification);
    resized();
}

void DrosakaVoiceEditor::timerCallback()
{
    const int gen = proc.bankGeneration.load();
    if (gen != seenGeneration)
    {
        seenGeneration = gen;
        shownBank = proc.getBank();
        // Rebuilding destroys the button that triggered this; do it from the timer, never inside onClick.
        rebuildRows();
        repaint();
    }

    const float ph = proc.getPlayheadProportion();
    if (! juce::exactlyEqual (ph, lastPlayhead))
    {
        lastPlayhead = ph;
        repaint (waveArea());
    }
    for (auto& r : rows) r->repaint();
}

juce::Rectangle<int> DrosakaVoiceEditor::waveArea() const
{
    return { 16, 50, getWidth() - 32 - 150, 96 };
}

void DrosakaVoiceEditor::resized()
{
    addButton.setBounds (getWidth() - 16 - 120, 14, 120, 26);
    sortFormant.setBounds (addButton.getX() - 12 - 70, 14, 70, 26);
    sortPitch.setBounds (sortFormant.getX() - 70, 14, 70, 26);

    auto wa = waveArea();
    countKnob.setBounds (wa.getRight() + 32, wa.getY(), 90, 90);

    listView.setBounds (16, wa.getBottom() + 50, getWidth() - 32, getHeight() - wa.getBottom() - 50 - 30);
    const int rowH = 26;
    listContent.setSize (listView.getMaximumVisibleWidth(), juce::jmax (1, (int) rows.size() * rowH));
    for (size_t i = 0; i < rows.size(); ++i)
        rows[i]->setBounds (0, (int) i * rowH, listContent.getWidth(), rowH);
}

void DrosakaVoiceEditor::paint (juce::Graphics& g)
{
    g.fillAll (colours::bg);

    g.setColour (colours::wave);
    g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    g.drawText ("drosaka voice", 16, 14, 300, 26, juce::Justification::centredLeft);
    g.setFont (juce::FontOptions (12.0f));
    g.setColour (colours::dim);
    g.drawText ("sort", sortPitch.getX() - 40, 14, 34, 26, juce::Justification::centredRight);

    // Primary waveform + playhead.
    auto area = waveArea();
    g.setColour (colours::panel);
    g.fillRoundedRectangle (area.toFloat(), 6.0f);
    if (dragHover)
    {
        g.setColour (colours::accent);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (2.0f), 6.0f, 2.0f);
    }

    const int n = shownBank != nullptr ? shownBank->size() : 0;
    const Stem* prim = (n > 0 && shownBank->primary >= 0) ? shownBank->stems[(size_t) shownBank->primary].get() : nullptr;

    if (prim == nullptr)
    {
        g.setColour (colours::dim);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText ("Drop voice stems anywhere on this window", area, juce::Justification::centred);
    }
    else
    {
        const auto& peaks = prim->peaks;
        const int bins = (int) peaks.size() / 2;
        const float midY = (float) area.getCentreY();
        const float halfH = (float) area.getHeight() * 0.45f;
        g.setColour (colours::wave);
        for (int x = 0; x < area.getWidth(); ++x)
        {
            const int b = juce::jlimit (0, bins - 1, x * bins / area.getWidth());
            g.drawVerticalLine (area.getX() + x, midY - peaks[(size_t) b * 2 + 1] * halfH, midY - peaks[(size_t) b * 2] * halfH + 1.0f);
        }
        if (lastPlayhead >= 0.0f)
        {
            g.setColour (colours::accent);
            g.fillRect (area.getX() + (int) (lastPlayhead * (float) area.getWidth()), area.getY(), 2, area.getHeight());
        }
        g.setColour (colours::dim);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("primary: " + prim->file.getFileName(), area.reduced (8, 4), juce::Justification::topLeft);
    }

    // Count readout.
    const float voices = (float) countKnob.getValue() * (float) n;
    g.setColour (colours::wave);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText ("COUNT", countKnob.getX(), countKnob.getBottom() - 2, countKnob.getWidth(), 16, juce::Justification::centred);
    g.setColour (colours::dim);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (juce::String (voices, 1) + " of " + juce::String (n), countKnob.getX(), countKnob.getBottom() + 14,
                countKnob.getWidth(), 14, juce::Justification::centred);

    // List header.
    g.setColour (colours::dim);
    g.drawText (juce::String (n) + (n == 1 ? " voice" : " voices") + "   low → high by "
                    + (shownBank != nullptr && shownBank->axis == SortAxis::formant ? "formant" : "pitch"),
                16, listView.getY() - 22, 400, 18, juce::Justification::centredLeft);
    const int infoRight = listView.getX() + listView.getMaximumVisibleWidth() - 4 * 26 - 16;   // matches VoiceRow
    g.drawText ("pitch       formant", infoRight - 200, listView.getY() - 22, 200, 18, juce::Justification::centredRight);

    // Footer: errors or missing files.
    auto footer = juce::Rectangle<int> (16, getHeight() - 26, getWidth() - 32, 22);
    const auto missing = proc.getMissingPaths();
    if (status.isNotEmpty())
    {
        g.setColour (colours::accent);
        g.drawText (status, footer, juce::Justification::centredLeft, true);
    }
    else if (! missing.isEmpty())
    {
        g.setColour (colours::accent);
        g.drawText ("Missing " + juce::String (missing.size()) + " file(s), drop them again: "
                        + juce::File (missing[0]).getFileName() + (missing.size() > 1 ? ", ..." : ""),
                    footer, juce::Justification::centredLeft, true);
    }
    else
    {
        g.setColour (colours::dim);
        g.drawText ("any MIDI note plays the whole bank in sync", footer, juce::Justification::centredLeft);
    }
}
