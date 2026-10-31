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
    const juce::Colour codec  { 0xff3fc8d0 };   // voices on the codec
    const juce::Colour warn   { 0xffe8a33d };   // couldn't auto-align
    const juce::Colour pitch  { 0xffb58cff };   // Spread
    const juce::Colour slice  { 0xffe8c547 };   // slice markers
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
// One stem in the sorted list:
//   [level + name] [alignment lane: drag to nudge] [codec order] [pitch formant] [P M S x]
class VoiceRow : public juce::Component
{
public:
    struct Cols { juce::Rectangle<int> name, lane, order, info, buttons; };

    static Cols columns (juce::Rectangle<int> r)
    {
        Cols c;
        c.buttons = r.removeFromRight (4 * 26 + 4);
        c.info = r.removeFromRight (150);
        c.order = r.removeFromRight (34);
        c.name = r.removeFromLeft (140);
        r.removeFromLeft (6);
        c.lane = r.reduced (0, 3);
        return c;
    }

    VoiceRow (DrosakaVoiceProcessor& p, std::function<Bank::Ptr()> bankGetter, Timeline& tl, const Bank& b, int stemIdx)
        : proc (p), getBank (std::move (bankGetter)), timeline (tl), idx (stemIdx)
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

        primaryBtn.setTooltip ("Primary voice (centre, alignment reference)");
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
        auto r = columns (getLocalBounds()).buttons.reduced (2);
        removeBtn.setBounds (r.removeFromRight (22).withSizeKeepingCentre (22, 22)); r.removeFromRight (4);
        soloBtn.setBounds (r.removeFromRight (22).withSizeKeepingCentre (22, 22));   r.removeFromRight (2);
        muteBtn.setBounds (r.removeFromRight (22).withSizeKeepingCentre (22, 22));   r.removeFromRight (2);
        primaryBtn.setBounds (r.removeFromRight (22).withSizeKeepingCentre (22, 22));
    }

    void paint (juce::Graphics& g) override
    {
        auto bank = getBank();
        if (bank == nullptr || idx >= bank->size()) return;
        const auto c = columns (getLocalBounds());
        g.setColour (colours::row);
        g.fillRect (getLocalBounds().reduced (0, 1));

        // Level (tinted by codec amount) behind the name.
        const float lvl = proc.getVoiceLevel (idx), enc = proc.getVoiceEncode (idx);
        g.setColour (colours::level.interpolatedWith (colours::codec.withAlpha (0.45f), enc));
        g.fillRect (c.name.reduced (0, 1).withWidth (juce::roundToInt (lvl * (float) c.name.getWidth())));
        if (isPrimary) { g.setColour (colours::accent); g.fillRect (c.name.reduced (0, 1).withWidth (3)); }
        g.setFont (juce::FontOptions (13.0f));
        g.setColour (colours::wave);
        g.drawText (name, c.name.withTrimmedLeft (10).withTrimmedRight (40), juce::Justification::centredLeft, true);
        const float semis = proc.getVoiceSemis (idx);
        if (std::abs (semis) > 0.04f)
        {
            g.setFont (juce::FontOptions (11.0f));
            g.setColour (colours::pitch);
            g.drawText ((semis > 0 ? "+" : "") + juce::String (semis, std::abs (semis) < 1.0f ? 2 : 1), c.name.withTrimmedRight (4),
                        juce::Justification::centredRight);
        }

        paintLane (g, c.lane, *bank);

        const int rank = proc.getVoiceEncodeRank (idx);
        g.setFont (juce::FontOptions (11.0f));
        g.setColour (enc > 0.5f ? colours::codec : colours::dim.withAlpha (0.6f));
        g.drawText (rank > 0 ? juce::String (rank) : juce::String ("-"), c.order, juce::Justification::centred);
        g.setColour (colours::dim);
        g.drawText (info, c.info.withTrimmedRight (8), juce::Justification::centredRight);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! columns (getLocalBounds()).lane.contains (e.getPosition())) return;
        auto bank = getBank();
        dragging = true;
        timeline.frozen = true;   // keep the scale still under the mouse
        dragStartOffset = bank->flags[(size_t) idx].offsetSec;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging) return;
        const auto lane = columns (getLocalBounds()).lane;
        const double secPerPx = (timeline.end - timeline.start) / juce::jmax (1, lane.getWidth());
        const double fine = e.mods.isShiftDown() ? 0.1 : 1.0;
        proc.setOffset (idx, dragStartOffset + e.getDistanceFromDragStartX() * secPerPx * fine);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        dragging = false;
        timeline.frozen = false;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (columns (getLocalBounds()).lane.contains (e.getPosition())) proc.resetOffset (idx);
    }

private:
    void drawPeaks (juce::Graphics& g, juce::Rectangle<int> lane, const Stem& s, double offset)
    {
        const double span = timeline.end - timeline.start;
        const double dur = s.audio.getNumSamples() / s.sampleRate;
        const int bins = (int) s.peaks.size() / 2;
        const float mid = (float) lane.getCentreY(), half = (float) lane.getHeight() * 0.48f;
        for (int x = 0; x < lane.getWidth(); ++x)
        {
            const double t = timeline.start + span * x / lane.getWidth() - offset;
            if (t < 0.0 || t >= dur) continue;
            const int b = juce::jlimit (0, bins - 1, (int) (t / dur * bins));
            const float lo = s.peaks[(size_t) b * 2], hi = s.peaks[(size_t) b * 2 + 1];
            g.drawVerticalLine (lane.getX() + x, mid - hi * half * 1.6f, mid - lo * half * 1.6f + 1.0f);
        }
    }

    void paintLane (juce::Graphics& g, juce::Rectangle<int> lane, const Bank& b)
    {
        g.setColour (colours::bg);
        g.fillRect (lane);
        g.saveState();
        g.reduceClipRegion (lane);

        const auto& f = b.flags[(size_t) idx];
        if (! isPrimary && b.primary >= 0)
        {
            g.setColour (colours::accent.withAlpha (0.28f));   // ghost of the primary, the reference
            drawPeaks (g, lane, *b.stems[(size_t) b.primary], b.flags[(size_t) b.primary].offsetSec);
        }
        const juce::Colour col = isPrimary ? colours::accent
                               : f.align == AlignState::unsure ? colours::warn
                               : colours::wave;
        g.setColour (col.withAlpha (0.9f));
        drawPeaks (g, lane, *b.stems[(size_t) idx], f.offsetSec);
        g.restoreState();

        // Status text.
        g.setFont (juce::FontOptions (10.0f));
        juce::String tag;
        if (isPrimary)                          tag = "reference";
        else if (f.align == AlignState::unsure) tag = "? not auto-aligned - drag";
        else if (f.align == AlignState::manual) tag = "manual";
        else                                    tag = "auto";
        const juce::String ms = juce::String (juce::roundToInt (f.offsetSec * 1000.0)) + " ms";
        g.setColour (f.align == AlignState::unsure && ! isPrimary ? colours::warn : colours::dim);
        g.drawText (tag, lane.reduced (4, 0), juce::Justification::topLeft);
        g.setColour (colours::dim);
        g.drawText ((f.offsetSec > 0 ? "+" : "") + ms, lane.reduced (4, 0), juce::Justification::topRight);
    }

    DrosakaVoiceProcessor& proc;
    std::function<Bank::Ptr()> getBank;
    Timeline& timeline;
    int idx;
    juce::String name, info;
    bool isPrimary = false, dragging = false;
    double dragStartOffset = 0.0;
    juce::TextButton primaryBtn { "P" }, muteBtn { "M" }, soloBtn { "S" }, removeBtn { "x" };
};

//==============================================================================
// The phrase, on the aligned timeline, with slice markers.
//   double-click: add a marker   drag: move it   right-click: delete it
// Region k plays on key C3 + k; any other key plays the whole phrase.
class SliceView : public juce::Component
{
public:
    SliceView (DrosakaVoiceProcessor& p, std::function<Bank::Ptr()> bankGetter) : proc (p), getBank (std::move (bankGetter)) {}

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds();
        g.setColour (colours::panel);
        g.fillRoundedRectangle (area.toFloat(), 6.0f);
        auto b = getBank();
        if (b == nullptr || b->size() == 0 || b->primary < 0)
        {
            g.setColour (colours::dim);
            g.setFont (juce::FontOptions (13.0f));
            g.drawText ("Drop voice stems anywhere on this window", area, juce::Justification::centred);
            return;
        }
        const double len = b->lengthSeconds();
        auto xOf = [&] (double t) { return (float) area.getX() + (float) (t / len) * (float) area.getWidth(); };

        // Regions, shaded alternately, labelled with their key.
        const int regions = b->numRegions();
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        for (int k = 0; k < regions; ++k)
        {
            const double a0 = k == 0 ? 0.0 : b->slices[(size_t) k - 1];
            const double a1 = k < (int) b->slices.size() ? b->slices[(size_t) k] : len;
            const auto r = juce::Rectangle<float> (xOf (a0), (float) area.getY(), xOf (a1) - xOf (a0), (float) area.getHeight());
            if (k % 2 == 1) { g.setColour (colours::slice.withAlpha (0.06f)); g.fillRect (r); }
            g.setColour (colours::slice);
            g.drawText (juce::MidiMessage::getMidiNoteName (Bank::sliceRoot + k, true, true, 3), r.reduced (5.0f, 3.0f).toNearestInt(),
                        juce::Justification::bottomLeft);
        }

        // The primary's waveform, at its place on the timeline.
        const auto& prim = *b->stems[(size_t) b->primary];
        const double off = b->flags[(size_t) b->primary].offsetSec;
        const double dur = prim.audio.getNumSamples() / prim.sampleRate;
        const int bins = (int) prim.peaks.size() / 2;
        const float midY = (float) area.getCentreY(), halfH = (float) area.getHeight() * 0.42f;
        g.setColour (colours::wave);
        for (int x = 0; x < area.getWidth(); ++x)
        {
            const double t = len * x / area.getWidth() - off;
            if (t < 0.0 || t >= dur) continue;
            const int bi = juce::jlimit (0, bins - 1, (int) (t / dur * bins));
            g.drawVerticalLine (area.getX() + x, midY - prim.peaks[(size_t) bi * 2 + 1] * halfH, midY - prim.peaks[(size_t) bi * 2] * halfH + 1.0f);
        }

        // Markers.
        for (size_t i = 0; i < b->slices.size(); ++i)
        {
            const float x = xOf (b->slices[i]);
            g.setColour ((int) i == dragging ? colours::wave : colours::slice);
            g.fillRect (x - 1.0f, (float) area.getY(), 2.0f, (float) area.getHeight());
            juce::Path tri;
            tri.addTriangle (x - 6.0f, (float) area.getY(), x + 6.0f, (float) area.getY(), x, (float) area.getY() + 8.0f);
            g.fillPath (tri);
        }

        const float ph = proc.getPlayheadProportion();
        if (ph >= 0.0f)
        {
            g.setColour (colours::accent);
            g.fillRect ((float) area.getX() + ph * (float) area.getWidth(), (float) area.getY(), 2.0f, (float) area.getHeight());
        }

        g.setFont (juce::FontOptions (11.0f));
        g.setColour (colours::dim);
        g.drawText ("primary: " + prim.file.getFileName(), area.reduced (8, 4), juce::Justification::topLeft);
        g.drawText (b->slices.empty() ? "double-click to slice"
                                      : juce::String (regions) + " slices from C3 - other keys play it all - drag / right-click markers",
                    area.reduced (8, 4), juce::Justification::topRight);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = markerNear (e.x);
        if (dragging >= 0 && e.mods.isPopupMenu())
        {
            proc.removeSlice (dragging);
            dragging = -1;
        }
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging >= 0) proc.moveSlice (dragging, timeAt (e.x));
    }

    void mouseUp (const juce::MouseEvent&) override { dragging = -1; repaint(); }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (markerNear (e.x) < 0) proc.addSlice (timeAt (e.x));
    }

private:
    double timeAt (int x) const
    {
        auto b = getBank();
        return b == nullptr ? 0.0 : juce::jlimit (0.0, 1.0, (double) x / juce::jmax (1, getWidth())) * b->lengthSeconds();
    }

    int markerNear (int x) const
    {
        auto b = getBank();
        if (b == nullptr) return -1;
        const double len = b->lengthSeconds();
        for (size_t i = 0; i < b->slices.size(); ++i)
            if (std::abs ((float) (b->slices[i] / len) * (float) getWidth() - (float) x) < 6.0f) return (int) i;
        return -1;
    }

    DrosakaVoiceProcessor& proc;
    std::function<Bank::Ptr()> getBank;
    int dragging = -1;
};

//==============================================================================
DrosakaVoiceEditor::DrosakaVoiceEditor (DrosakaVoiceProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    sliceView = std::make_unique<SliceView> (proc, [this] { return shownBank; });
    addAndMakeVisible (*sliceView);

    addAndMakeVisible (presetButton);
    presetButton.onClick = [this]
    {
        juce::PopupMenu m;
        m.addItem (1, "Save preset (bank + audio + slices + knobs)...");
        m.addItem (2, "Load preset...");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (presetButton), [this] (int r)
        {
            if (r == 0) return;
            const bool save = r == 1;
            chooser = std::make_unique<juce::FileChooser> (save ? "Save drosaka Voice preset" : "Load drosaka Voice preset",
                                                           juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.dvoice");
            const int chooserFlags = save ? (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting)
                                   : (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles);
            chooser->launchAsync (chooserFlags, [this, save] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (f == juce::File()) return;
                if (save) { f = f.withFileExtension ("dvoice"); status = proc.savePreset (f) ? juce::String() : "Couldn't save " + f.getFileName(); }
                else status = proc.loadPreset (f) ? juce::String() : "Couldn't load " + f.getFileName();
                repaint();
            });
        });
    };

    addAndMakeVisible (alignButton);
    alignButton.setTooltip ("Auto-align every stem to the primary again (overwrites manual nudges)");
    alignButton.onClick = [this] { proc.alignAll(); };

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

    // Codec strip.
    for (auto* k : { &encodeKnob, &bitRateKnob, &frameDropKnob })
    {
        k->setColour (juce::Slider::rotarySliderFillColourId, colours::accent);
        k->setColour (juce::Slider::rotarySliderOutlineColourId, colours::row);
        k->setColour (juce::Slider::thumbColourId, colours::wave);
        k->onValueChange = [this] { repaint (codecArea()); };
        addAndMakeVisible (*k);
    }
    encodeKnob.setColour (juce::Slider::rotarySliderFillColourId, colours::codec);
    encodeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.params, "encodeCount", encodeKnob);
    bitRateAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.params, "bitRate", bitRateKnob);
    frameDropAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.params, "frameDrop", frameDropKnob);

    pitchLockBox.addItemList ({ "Fixed", "Free", "Tracking" }, 1);
    pitchLockBox.setColour (juce::ComboBox::backgroundColourId, colours::row);
    pitchLockBox.setColour (juce::ComboBox::outlineColourId, colours::row);
    pitchLockBox.setColour (juce::ComboBox::textColourId, colours::wave);
    addAndMakeVisible (pitchLockBox);
    pitchLockAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.params, "pitchLock", pitchLockBox);

    // Spread strip.
    for (auto* k : { &spreadKnob, &snapKnob })
    {
        k->setColour (juce::Slider::rotarySliderFillColourId, colours::pitch);
        k->setColour (juce::Slider::rotarySliderOutlineColourId, colours::row);
        k->setColour (juce::Slider::thumbColourId, colours::wave);
        k->onValueChange = [this] { repaint (spreadArea()); };
        addAndMakeVisible (*k);
    }
    spreadAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.params, "spread", spreadKnob);
    snapAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.params, "snap", snapKnob);
    for (int i = 0; i < spread::numChordSets; ++i) chordBox.addItem (spread::chordSets()[i].name, i + 1);
    chordBox.setColour (juce::ComboBox::backgroundColourId, colours::row);
    chordBox.setColour (juce::ComboBox::outlineColourId, colours::row);
    chordBox.setColour (juce::ComboBox::textColourId, colours::wave);
    addAndMakeVisible (chordBox);
    chordAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.params, "chord", chordBox);

    // Space & time strip.
    auto setupBox = [this] (juce::ComboBox& box, const juce::StringArray& items, const char* paramId,
                            std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>& att)
    {
        box.addItemList (items, 1);
        box.setColour (juce::ComboBox::backgroundColourId, colours::row);
        box.setColour (juce::ComboBox::outlineColourId, colours::row);
        box.setColour (juce::ComboBox::textColourId, colours::wave);
        addAndMakeVisible (box);
        att = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.params, paramId, box);
    };
    setupBox (spatialBox, spatial::modeNames(), "spatialMode", spatialAttachment);
    setupBox (motionBox, spatial::divisionNames(), "motionDiv", motionAttachment);
    setupBox (quantBox, spatial::quantiseNames(), "encodeQuant", quantAttachment);

    listView.setViewedComponent (&listContent, false);
    listView.setScrollBarsShown (true, false);
    addAndMakeVisible (listView);

    setSize (860, 796);
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
        rows.push_back (std::make_unique<VoiceRow> (proc, [this] { return shownBank; }, timeline, *shownBank, s));
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
        updateTimeline();
        const int sgen = proc.structureGeneration.load();
        if (sgen != seenStructure)
        {
            seenStructure = sgen;
            // Rebuilding destroys the button that triggered this; do it from the timer, never inside onClick.
            rebuildRows();
        }
        repaint();
        sliceView->repaint();
    }
    else if (! timeline.frozen && timelineDirty)
    {
        updateTimeline();
    }

    const float ph = proc.getPlayheadProportion();
    if (! juce::exactlyEqual (ph, lastPlayhead))
    {
        lastPlayhead = ph;
        sliceView->repaint();
    }
    for (auto& r : rows) r->repaint();
    repaint (fieldArea());
    repaint (spaceArea().withTrimmedRight (240));
}

void DrosakaVoiceEditor::updateTimeline()
{
    if (timeline.frozen) { timelineDirty = true; return; }   // a drag is in progress
    timelineDirty = false;
    if (shownBank == nullptr || shownBank->size() == 0) { timeline.start = 0.0; timeline.end = 1.0; return; }
    double a = 1e9, b = -1e9;
    for (int i = 0; i < shownBank->size(); ++i)
    {
        const auto& s = *shownBank->stems[(size_t) i];
        const double off = shownBank->flags[(size_t) i].offsetSec;
        a = juce::jmin (a, off);
        b = juce::jmax (b, off + s.audio.getNumSamples() / s.sampleRate);
    }
    const double pad = (b - a) * 0.03;
    timeline.start = a - pad;
    timeline.end = b + pad;
    for (auto& r : rows) r->repaint();
}

juce::Rectangle<int> DrosakaVoiceEditor::waveArea() const
{
    return { 16, 50, getWidth() - 32 - 150, 96 };
}

juce::Rectangle<int> DrosakaVoiceEditor::codecArea() const
{
    return { 16, waveArea().getBottom() + 40, getWidth() - 32, 66 };
}

juce::Rectangle<int> DrosakaVoiceEditor::spreadArea() const
{
    return codecArea().translated (0, 76);
}

juce::Rectangle<int> DrosakaVoiceEditor::spaceArea() const
{
    return spreadArea().translated (0, 76);
}

juce::Rectangle<int> DrosakaVoiceEditor::fieldArea() const
{
    auto a = spaceArea().reduced (10, 8);
    return a.removeFromRight (230);
}

void DrosakaVoiceEditor::resized()
{
    addButton.setBounds (getWidth() - 16 - 120, 14, 120, 26);
    sortFormant.setBounds (addButton.getX() - 12 - 70, 14, 70, 26);
    sortPitch.setBounds (sortFormant.getX() - 70, 14, 70, 26);
    alignButton.setBounds (sortPitch.getX() - 60 - 90, 14, 90, 26);
    presetButton.setBounds (alignButton.getX() - 10 - 80, 14, 80, 26);
    sliceView->setBounds (waveArea());

    auto wa = waveArea();
    countKnob.setBounds (wa.getRight() + 32, wa.getY(), 90, 90);

    auto ca = codecArea();
    encodeKnob.setBounds (ca.getX() + 10, ca.getY() + 4, 56, 56);
    bitRateKnob.setBounds (ca.getX() + 200, ca.getY() + 8, 46, 46);
    frameDropKnob.setBounds (ca.getX() + 340, ca.getY() + 8, 46, 46);
    pitchLockBox.setBounds (ca.getX() + 490, ca.getCentreY() - 6, 120, 26);

    auto sa = spreadArea();
    spreadKnob.setBounds (sa.getX() + 10, sa.getY() + 4, 56, 56);
    chordBox.setBounds (sa.getX() + 200, sa.getCentreY() - 6, 150, 26);
    snapKnob.setBounds (sa.getX() + 400, sa.getY() + 8, 46, 46);

    auto xa = spaceArea();
    spatialBox.setBounds (xa.getX() + 10, xa.getY() + 28, 120, 26);
    motionBox.setBounds (xa.getX() + 140, xa.getY() + 28, 90, 26);
    quantBox.setBounds (xa.getX() + 270, xa.getY() + 28, 90, 26);

    listView.setBounds (16, xa.getBottom() + 34, getWidth() - 32, getHeight() - xa.getBottom() - 34 - 30);
    const int rowH = 34;
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

    if (dragHover)
    {
        g.setColour (colours::accent);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (2.0f), 6.0f, 2.0f);
    }
    const int n = shownBank != nullptr ? shownBank->size() : 0;

    // Codec strip.
    {
        auto ca = codecArea();
        g.setColour (colours::panel);
        g.fillRoundedRectangle (ca.toFloat(), 6.0f);
        g.setFont (juce::FontOptions (11.0f));
        auto label = [&] (juce::Component& c, const juce::String& name, const juce::String& value)
        {
            g.setColour (colours::wave);
            g.drawText (name, c.getRight() + 8, ca.getY() + 12, 90, 14, juce::Justification::centredLeft);
            g.setColour (colours::dim);
            g.drawText (value, c.getRight() + 8, ca.getY() + 28, 90, 14, juce::Justification::centredLeft);
        };
        {
            const int nVoices = proc.audibleVoices.load();
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            g.setColour (colours::codec);
            g.drawText ("ENCODE", encodeKnob.getRight() + 8, ca.getY() + 12, 100, 16, juce::Justification::centredLeft);
            g.setFont (juce::FontOptions (11.0f));
            g.setColour (colours::dim);
            g.drawText (juce::String (encodeKnob.getValue() * nVoices, 1) + " of " + juce::String (nVoices) + " audible",
                        encodeKnob.getRight() + 8, ca.getY() + 30, 110, 14, juce::Justification::centredLeft);
            g.setFont (juce::FontOptions (11.0f));
        }
        label (bitRateKnob, "BIT RATE", bitRateKnob.getValue() < 0.02 ? "clean" : juce::String (juce::roundToInt (bitRateKnob.getValue() * 100)) + "% crushed");
        label (frameDropKnob, "FRAME DROP", juce::String (juce::roundToInt (frameDropKnob.getValue() * 50)) + "% loss");
        g.setColour (colours::wave);
        g.drawText ("PITCH LOCK", pitchLockBox.getX(), ca.getY() + 6, 120, 14, juce::Justification::centredLeft);
        g.setColour (colours::dim);
        g.drawText ("8 kHz LPC-10", ca.getRight() - 110, ca.getY() + 6, 100, 14, juce::Justification::centredRight);
    }

    // Spread strip.
    {
        auto sa = spreadArea();
        g.setColour (colours::panel);
        g.fillRoundedRectangle (sa.toFloat(), 6.0f);
        const float v = (float) spreadKnob.getValue();
        const char* names[] = { "unison", "chorus", "chord", "cluster" };
        const float pos = v * 3.0f;
        const int near = juce::jmin (3, juce::roundToInt (pos));
        const juce::String where = std::abs (pos - (float) near) < 0.04f ? juce::String (names[near])
                                 : juce::String (names[juce::jmin (2, (int) pos)]) + juce::String (juce::CharPointer_UTF8 (" \xe2\x86\x92 ")) + names[juce::jmin (3, (int) pos + 1)];
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.setColour (colours::pitch);
        g.drawText ("SPREAD", spreadKnob.getRight() + 8, sa.getY() + 12, 110, 16, juce::Justification::centredLeft);
        g.setFont (juce::FontOptions (11.0f));
        g.setColour (colours::dim);
        g.drawText (where, spreadKnob.getRight() + 8, sa.getY() + 30, 120, 14, juce::Justification::centredLeft);
        g.setColour (colours::wave);
        g.drawText ("CHORD", chordBox.getX(), sa.getY() + 6, 120, 14, juce::Justification::centredLeft);
        g.drawText ("SNAP", snapKnob.getRight() + 8, sa.getY() + 12, 90, 14, juce::Justification::centredLeft);
        g.setColour (colours::dim);
        const float sn = (float) snapKnob.getValue();
        g.drawText (sn < 0.05f ? "portamento" : sn > 0.9f ? "near-stepped" : juce::String (juce::roundToInt (sn * 100)) + "% linger",
                    snapKnob.getRight() + 8, sa.getY() + 28, 100, 14, juce::Justification::centredLeft);
        g.drawText ("dry voices +-7 st, codec plays the rest", sa.getRight() - 250, sa.getY() + 6, 240, 14, juce::Justification::centredRight);
    }

    // Space & time strip, with a live view of the stereo field.
    {
        auto xa = spaceArea();
        g.setColour (colours::panel);
        g.fillRoundedRectangle (xa.toFloat(), 6.0f);
        g.setFont (juce::FontOptions (11.0f));
        g.setColour (colours::wave);
        g.drawText ("SPACE", spatialBox.getX(), xa.getY() + 8, 120, 14, juce::Justification::centredLeft);
        g.drawText ("RATE", motionBox.getX(), xa.getY() + 8, 90, 14, juce::Justification::centredLeft);
        g.drawText ("ENCODE STEPS", quantBox.getX(), xa.getY() + 8, 100, 14, juce::Justification::centredLeft);
        g.setColour (colours::dim);
        g.drawText (juce::String (proc.hostBpm.load(), 1) + " bpm  " + (proc.hostPlaying.load() ? "Live" : "internal clock"),
                    quantBox.getRight() + 16, xa.getY() + 34, 160, 14, juce::Justification::centredLeft);

        auto f = fieldArea();
        g.setColour (colours::bg);
        g.fillRoundedRectangle (f.toFloat(), 4.0f);
        g.setColour (colours::row);
        g.drawVerticalLine (f.getCentreX(), (float) f.getY() + 2, (float) f.getBottom() - 2);
        g.setColour (colours::dim);
        g.drawText ("L", f.reduced (4, 0), juce::Justification::centredLeft);
        g.drawText ("R", f.reduced (4, 0), juce::Justification::centredRight);
        if (shownBank != nullptr)
        {
            const int nv = shownBank->size();
            for (int r = 0; r < nv; ++r)
            {
                const int idx = shownBank->sorted[(size_t) r];
                const float lvl = proc.getVoiceLevel (idx);
                if (lvl <= 0.01f) continue;
                const float x = (float) f.getCentreX() + proc.getVoicePan (idx) * (float) (f.getWidth() / 2 - 14);
                const float y = (float) f.getBottom() - 6.0f - (float) (f.getHeight() - 12) * (nv > 1 ? (float) r / (float) (nv - 1) : 0.5f);
                const auto c = idx == shownBank->primary ? colours::accent
                                                         : colours::wave.interpolatedWith (colours::codec, proc.getVoiceEncode (idx));
                g.setColour (c.withAlpha (0.4f + 0.6f * juce::jmin (1.0f, lvl * 1.5f)));
                g.fillEllipse (x - 4.0f, y - 3.0f, 8.0f, 6.0f);
            }
        }
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
    g.drawText (juce::String (n) + (n == 1 ? " voice" : " voices") + juce::String (juce::CharPointer_UTF8 ("  low \xe2\x86\x92 high")),
                16, listView.getY() - 22, 140, 18, juce::Justification::centredLeft);
    {
        auto cols = VoiceRow::columns ({ listView.getX(), listView.getY() - 22, listView.getMaximumVisibleWidth(), 18 });
        g.drawText ("pitch       formant", cols.info.withTrimmedRight (8), juce::Justification::centredRight);
        g.drawText ("codec", cols.order, juce::Justification::centred);
        g.drawText ("alignment to primary  (drag, shift = fine, double-click = reset)", cols.lane, juce::Justification::centredLeft);
    }

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
        const int regions = shownBank != nullptr ? shownBank->numRegions() : 0;
        g.drawText (regions > 0 ? "C3 to " + juce::MidiMessage::getMidiNoteName (Bank::sliceRoot + regions - 1, true, true, 3)
                                    + " play the slices across every voice; any other key plays the whole phrase"
                                : juce::String ("any MIDI note plays the whole bank in sync; double-click the waveform to slice"),
                    footer, juce::Justification::centredLeft);
    }
}
