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
        const float note = proc.getVoiceNote (idx);   // the note Spread has this voice singing
        if (note >= 0.0f)
        {
            const int nn = juce::roundToInt (note);
            const int cents = juce::roundToInt ((note - (float) nn) * 100.0f);
            g.setFont (juce::FontOptions (11.0f));
            g.setColour (colours::pitch);
            g.drawText (juce::MidiMessage::getMidiNoteName (nn, true, true, 3) + (std::abs (cents) >= 3 ? (cents > 0 ? " +" : " ") + juce::String (cents) : juce::String()),
                        c.name.withTrimmedRight (4), juce::Justification::centredRight);
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
        const auto sl = proc.getSlices();
        const int regions = sl.numRegions();
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        for (int k = 0; k < regions; ++k)
        {
            const double a0 = sl.t[k];
            const double a1 = k + 1 < sl.n ? sl.t[k + 1] : len;
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

        // Before the first marker is dead space (never played): dimmed over the waveform.
        if (sl.n > 0)
        {
            g.setColour (colours::bg.withAlpha (0.55f));
            g.fillRect (juce::Rectangle<float> ((float) area.getX(), (float) area.getY(), xOf (sl.t[0]) - (float) area.getX(), (float) area.getHeight()));
        }

        // Markers.
        for (int i = 0; i < sl.n; ++i)
        {
            // Automation offset: a faint line at the marker's home, joined to where it is now.
            if (std::abs (sl.t[i] - sl.base[i]) > 0.001)
            {
                const float hx = xOf (sl.base[i]);
                g.setColour (colours::slice.withAlpha (0.35f));
                g.drawVerticalLine ((int) hx, (float) area.getY() + 10.0f, (float) area.getBottom());
                g.drawHorizontalLine (area.getY() + 10, juce::jmin (hx, xOf (sl.t[i])), juce::jmax (hx, xOf (sl.t[i])));
            }
            const float x = xOf (sl.t[i]);
            g.setColour (i == dragging ? colours::wave : colours::slice);
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
        g.drawText (sl.n == 0 ? "double-click to slice"
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
        const auto sl = proc.getSlices();
        for (int i = 0; i < sl.n; ++i)
            if (std::abs ((float) (sl.t[i] / len) * (float) getWidth() - (float) x) < 6.0f) return i;
        return -1;
    }

    DrosakaVoiceProcessor& proc;
    std::function<Bank::Ptr()> getBank;
    int dragging = -1;
};

//==============================================================================
// One cell of the 4x4 control grid: name, knob, value, and its MIDI CC.
// Right-click: MIDI learn / forget. Double-click: back to default.
class LearnSlider : public juce::Slider
{
public:
    LearnSlider() : juce::Slider (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox) {}
    std::function<void()> onRightClick;
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { if (onRightClick) onRightClick(); return; }
        juce::Slider::mouseDown (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu()) juce::Slider::mouseDrag (e); }
    void mouseUp (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu()) juce::Slider::mouseUp (e); }
};

class KnobCell : public juce::Component
{
public:
    KnobCell (DrosakaVoiceProcessor& p, const juce::String& id, const juce::String& title, juce::Colour c,
              std::function<juce::String()> valueText)
        : proc (p), paramId (id), name (title), colour (c), value (std::move (valueText))
    {
        knob.setColour (juce::Slider::rotarySliderFillColourId, colour);
        knob.setColour (juce::Slider::rotarySliderOutlineColourId, colours::row);
        knob.setColour (juce::Slider::thumbColourId, colours::wave);
        knob.onValueChange = [this] { repaint(); };
        knob.onRightClick = [this] { showMenu(); };
        addAndMakeVisible (knob);
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.params, paramId, knob);
        if (auto* prm = proc.params.getParameter (paramId))
            knob.setDoubleClickReturnValue (true, prm->convertFrom0to1 (prm->getDefaultValue()));
        if (! value) value = [this] { return proc.params.getParameter (paramId)->getCurrentValueAsText(); };
    }

    void resized() override { knob.setBounds (getLocalBounds().withSizeKeepingCentre (46, 46).translated (0, 1)); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().reduced (2);
        const bool learning = proc.getLearningParam() == paramId;
        g.setColour (colours::panel);
        g.fillRoundedRectangle (r.toFloat(), 5.0f);
        if (learning)
        {
            g.setColour (colours::slice);
            g.drawRoundedRectangle (r.toFloat().reduced (1.0f), 5.0f, 2.0f);
        }
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.setColour (colour);
        g.drawText (name, r.reduced (7, 3), juce::Justification::topLeft);
        const int cc = proc.getCcFor (paramId);
        g.setFont (juce::FontOptions (9.5f));
        g.setColour (learning ? colours::slice : colours::dim);
        g.drawText (learning ? "learn" : (cc >= 0 ? "CC " + juce::String (cc) : juce::String()), r.reduced (6, 4), juce::Justification::topRight);
        g.setFont (juce::FontOptions (11.0f));
        g.setColour (colours::dim);
        g.drawText (value(), r.reduced (6, 3), juce::Justification::centredBottom);
    }

private:
    void showMenu()
    {
        juce::PopupMenu m;
        const int cc = proc.getCcFor (paramId);
        m.addSectionHeader (name);
        m.addItem (1, proc.getLearningParam() == paramId ? "Cancel MIDI learn" : "MIDI learn (then turn a control)");
        m.addItem (2, cc >= 0 ? "Forget CC " + juce::String (cc) : "Forget MIDI mapping", cc >= 0);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [this] (int r)
        {
            if (r == 1) { if (proc.getLearningParam() == paramId) proc.cancelMidiLearn(); else proc.armMidiLearn (paramId); }
            if (r == 2) proc.forgetCc (paramId);
            if (auto* parent = getParentComponent()) parent->repaint();
        });
    }

    DrosakaVoiceProcessor& proc;
    juce::String paramId, name;
    juce::Colour colour;
    std::function<juce::String()> value;
    LearnSlider knob;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

//==============================================================================
DrosakaVoiceEditor::DrosakaVoiceEditor (DrosakaVoiceProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    sliceView = std::make_unique<SliceView> (proc, [this] { return shownBank; });
    addAndMakeVisible (*sliceView);

    for (int k = 0; k < DrosakaVoiceProcessor::numBanks; ++k)
    {
        auto& bb = bankButtons[k];
        bb.setButtonText (juce::String (k + 1));
        bb.setTooltip ("Bank " + juce::String (k + 1) + ": its own stems, slices and settings. Switches on the next bar (at once when stopped)");
        bb.setConnectedEdges ((k > 0 ? juce::Button::ConnectedOnLeft : 0) | (k < DrosakaVoiceProcessor::numBanks - 1 ? juce::Button::ConnectedOnRight : 0));
        bb.onClick = [this, k]
        {
            auto* prm = proc.params.getParameter ("bank");
            prm->beginChangeGesture();
            prm->setValueNotifyingHost (prm->convertTo0to1 ((float) k));
            prm->endChangeGesture();
            updateBankButtons();
        };
        addAndMakeVisible (bb);
    }
    updateBankButtons();
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

    // The 4x4 grid: every continuous control plus the stepped ones you play.
    auto pct = [this] (const char* id) { return [this, id] { return juce::String (juce::roundToInt (proc.params.getParameter (id)->getValue() * 100.0f)) + "%"; }; };
    auto voices = [this] { return shownBank != nullptr ? shownBank->size() : 0; };
    struct Def { const char* id; const char* title; juce::Colour c; std::function<juce::String()> v; };
    const Def defs[] = {
        { "count",       "COUNT",      colours::accent, [this, voices] { return juce::String (proc.params.getRawParameterValue ("count")->load() * (float) voices(), 1) + " of " + juce::String (voices()); } },
        { "spread",      "SPREAD",     colours::pitch,  [this]
            {
                juce::StringArray held;   // harmony keys (C1-B1) override the chord menu
                for (int pc = 0; pc < 12; ++pc)
                    if (proc.isHarmonyHeld (pc)) held.add (juce::MidiMessage::getMidiNoteName (pc, true, false, 3));
                if (! held.isEmpty()) return "keys: " + held.joinIntoString (" ");
                const float v = proc.params.getRawParameterValue ("spread")->load() * 3.0f;
                const char* names[] = { "natural", "chorus", "chord", "cluster" };
                const int near = juce::jmin (3, juce::roundToInt (v));
                return std::abs (v - (float) near) < 0.04f ? juce::String (names[near])
                     : juce::String (names[juce::jmin (2, (int) v)]) + juce::String (juce::CharPointer_UTF8 (" \xe2\x86\x92 ")) + names[juce::jmin (3, (int) v + 1)];
            } },
        { "snap",        "SNAP",       colours::pitch,  {} },
        { "chord",       "CHORD",      colours::pitch,  {} },
        { "encodeCount", "ENCODE",     colours::codec,  [this] { return juce::String (proc.params.getRawParameterValue ("encodeCount")->load() * (float) proc.audibleVoices.load(), 1) + " of " + juce::String (proc.audibleVoices.load()); } },
        { "bitRate",     "BIT RATE",   colours::codec,  [this] { const float v = proc.params.getRawParameterValue ("bitRate")->load(); return v < 0.02f ? juce::String ("clean") : juce::String (juce::roundToInt (v * 100.0f)) + "% crushed"; } },
        { "frameDrop",   "FRAME DROP", colours::codec,  [this] { return juce::String (juce::roundToInt (proc.params.getRawParameterValue ("frameDrop")->load() * 50.0f)) + "% loss"; } },
        { "depth",       "DEPTH",      colours::wave,   [this] { const float v = proc.params.getRawParameterValue ("depth")->load(); return v < 0.01f ? juce::String ("pure pan") : juce::String (juce::roundToInt (v * 100.0f)) + "%"; } },
        { "spatialMode", "SPACE",      colours::wave,   {} },
        { "motionDiv",   "RATE",       colours::wave,   {} },
        { "scheme",      "SCHEME",     colours::slice,  {} },
        { "patternDiv",  "DIVISION",   colours::slice,  {} },
        { "phraseLength","LENGTH",     colours::slice,  [this]
            {
                const int R = juce::jmax (1, proc.getSlices().numRegions());
                const int L = 1 + (int) std::lround (proc.params.getRawParameterValue ("phraseLength")->load() * (float) (R - 1));
                return juce::String (L) + " of " + juce::String (R) + (R == 1 ? " slice" : " slices");
            } },
        { "fill",        "FILL",       colours::slice,  [this]
            {
                const int N = juce::jmax (1, (int) std::lround (4.0 / pattern::divisionBeats ((int) proc.params.getRawParameterValue ("patternDiv")->load())));
                const int k = juce::jmax (1, (int) std::lround (proc.params.getRawParameterValue ("fill")->load() * (float) N));
                return juce::String (k) + " of " + juce::String (N) + " steps";
            } },
        { "stutter",     "STUTTER",    colours::slice,  pct ("stutter") },
        { "variation",   "VARIATION",  colours::slice,  pct ("variation") },
    };
    for (auto& d : defs)
    {
        cells.push_back (std::make_unique<KnobCell> (proc, d.id, d.title, d.c, d.v));
        addAndMakeVisible (*cells.back());
    }

    // Performance panel: modes and switches.
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
    setupBox (playModeBox, juce::StringArray { "Manual", "Pattern" }, "playMode", playModeAttachment);
    setupBox (pitchLockBox, juce::StringArray { "Fixed", "Free", "Tracking" }, "pitchLock", pitchLockAttachment);
    setupBox (quantBox, spatial::quantiseNames(), "encodeQuant", quantAttachment);

    auto styleSwitch = [this] (juce::TextButton& b2, juce::Colour on, const char* paramId,
                               std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>& att)
    {
        b2.setClickingTogglesState (true);
        b2.setColour (juce::TextButton::buttonColourId, colours::row);
        b2.setColour (juce::TextButton::buttonOnColourId, on);
        b2.setColour (juce::TextButton::textColourOffId, colours::dim);
        b2.setColour (juce::TextButton::textColourOnId, colours::bg);
        addAndMakeVisible (b2);
        att = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.params, paramId, b2);
    };
    styleSwitch (dropButton, colours::accent, "drop", dropAttachment);
    dropButton.setTooltip ("The drop: the crowd fades out and the primary comes back human, dead centre");
    styleSwitch (keepHumanButton, colours::accent, "keepHuman", keepHumanAttachment);
    keepHumanButton.setTooltip ("Encode never takes the primary: 100% = every voice but the real one");
    styleSwitch (latchButton, colours::slice, "latch", latchAttachment);
    latchButton.setTooltip ("The pattern keeps looping after you let go of the key");

    // Autotune and recording.
    // Pattern feel, in the Performance panel.
    for (auto [id, title] : { std::pair<const char*, const char*> { "gate", "GATE" }, { "swing", "SWING" }, { "accent", "ACCENT" }, { "dropSemis", "SCREW DROP" } })
    {
        feelCells.push_back (std::make_unique<KnobCell> (proc, id, title, colours::slice, std::function<juce::String()> {}));
        addAndMakeVisible (*feelCells.back());
    }

    tuneCell = std::make_unique<KnobCell> (proc, "tuneAmount", "TUNE", colours::pitch, std::function<juce::String()> {});
    addAndMakeVisible (*tuneCell);
    driftCell = std::make_unique<KnobCell> (proc, "drift", "DRIFT", colours::pitch, std::function<juce::String()> {});
    addAndMakeVisible (*driftCell);
    setupBox (tuneRootBox, tune::rootNames(), "tuneRoot", tuneRootAttachment);
    setupBox (tuneScaleBox, tune::scaleNames(), "tuneScale", tuneScaleAttachment);
    styleSwitch (recordButton, juce::Colour (0xffe0303a), "record", recordAttachment);
    recordButton.setTooltip ("Record a take from this device's Sidechain input (pick the source track in Live's Sidechain menu on the device)");

    listView.setViewedComponent (&listContent, false);
    listView.setScrollBarsShown (true, false);
    addAndMakeVisible (listView);

    setSize (860, 960);
    startTimerHz (30);
}

DrosakaVoiceEditor::~DrosakaVoiceEditor()
{
    rows.clear();
    cells.clear();
    tuneCell.reset();
    feelCells.clear();
    driftCell.reset();
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
    for (auto& c : cells) c->repaint();   // values that follow the music (Encode of N audible...) and CC badges
    sliceView->repaint();                 // markers can be moved by automation
    updateBankButtons();
    if (tuneCell != nullptr) tuneCell->repaint();
    if (driftCell != nullptr) driftCell->repaint();
    for (auto& c : feelCells) c->repaint();
    if (auto msg = proc.getRecordMessage(); msg != lastRecordMessage) { lastRecordMessage = msg; status = msg; repaint(); }
    repaint (panelArea());
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

juce::Rectangle<int> DrosakaVoiceEditor::waveArea() const   { return { 16, 50, getWidth() - 32, 100 }; }
juce::Rectangle<int> DrosakaVoiceEditor::gridArea() const   { return { 16, waveArea().getBottom() + 30, 4 * 112, 4 * cellH }; }
juce::Rectangle<int> DrosakaVoiceEditor::panelArea() const
{
    auto g = gridArea();
    return { g.getRight() + 14, g.getY(), getWidth() - 16 - g.getRight() - 14, g.getHeight() };
}
juce::Rectangle<int> DrosakaVoiceEditor::fieldArea() const
{
    auto p = panelArea().reduced (10);
    return p.removeFromBottom (112);
}

void DrosakaVoiceEditor::resized()
{
    addButton.setBounds (getWidth() - 16 - 120, 14, 120, 26);
    sortFormant.setBounds (addButton.getX() - 12 - 70, 14, 70, 26);
    sortPitch.setBounds (sortFormant.getX() - 70, 14, 70, 26);
    alignButton.setBounds (sortPitch.getX() - 60 - 90, 14, 90, 26);
    presetButton.setBounds (alignButton.getX() - 10 - 80, 14, 80, 26);
    for (int k = 0; k < DrosakaVoiceProcessor::numBanks; ++k)
        bankButtons[k].setBounds (196 + k * 30, 14, 30, 26);
    sliceView->setBounds (waveArea());

    auto g = gridArea();
    for (size_t i = 0; i < cells.size(); ++i)
        cells[i]->setBounds (g.getX() + (int) (i % 4) * 112, g.getY() + (int) (i / 4) * cellH, 112, cellH);

    auto p = panelArea().reduced (10);
    const int w3 = (p.getWidth() - 16) / 3;
    playModeBox.setBounds (p.getX(), p.getY() + 18, w3, 26);
    pitchLockBox.setBounds (p.getX() + w3 + 8, p.getY() + 18, w3, 26);
    quantBox.setBounds (p.getX() + 2 * (w3 + 8), p.getY() + 18, w3, 26);
    dropButton.setBounds (p.getX(), p.getY() + 70, w3, 34);
    keepHumanButton.setBounds (p.getX() + w3 + 8, p.getY() + 70, w3, 34);
    latchButton.setBounds (p.getX() + 2 * (w3 + 8), p.getY() + 70, w3, 34);
    keepHumanButton.setButtonText ("KEEP HUMAN");

    tuneCell->setBounds (p.getX() - 2, p.getY() + 110, 92, 84);
    driftCell->setBounds (p.getX() + 90, p.getY() + 110, 92, 84);
    tuneRootBox.setBounds (p.getX() + 186, p.getY() + 124, 66, 24);
    tuneScaleBox.setBounds (p.getX() + 186, p.getY() + 168, 66, 24);
    recordButton.setBounds (p.getRight() - 84, p.getY() + 124, 84, 34);
    const int fw = p.getWidth() / 4;
    for (size_t i = 0; i < feelCells.size(); ++i)
        feelCells[i]->setBounds (p.getX() + (int) i * fw, p.getY() + 200, fw, 84);

    listView.setBounds (16, g.getBottom() + 34, getWidth() - 32, getHeight() - g.getBottom() - 34 - 30);
    const int rowH = 34;
    listContent.setSize (listView.getMaximumVisibleWidth(), juce::jmax (1, (int) rows.size() * rowH));
    for (size_t i = 0; i < rows.size(); ++i)
        rows[i]->setBounds (0, (int) i * rowH, listContent.getWidth(), rowH);
}

void DrosakaVoiceEditor::updateBankButtons()
{
    const int active = proc.getActiveBank(), target = proc.getBankTarget();
    ++bankBlink;
    for (int k = 0; k < DrosakaVoiceProcessor::numBanks; ++k)
    {
        auto& bb = bankButtons[k];
        const bool playing = k == active, waiting = k == target && target != active;
        const bool empty = proc.getBankStemCount (k) == 0;
        const auto bg = playing ? colours::accent
                      : waiting ? colours::accent.withAlpha ((bankBlink / 8) % 2 == 0 ? 0.55f : 0.2f)
                                : colours::panel;
        bb.setColour (juce::TextButton::buttonColourId, bg);
        bb.setColour (juce::TextButton::textColourOffId, playing ? colours::bg : empty ? colours::dim : colours::wave);
    }
}

void DrosakaVoiceEditor::paint (juce::Graphics& g)
{
    g.fillAll (colours::bg);

    g.setColour (colours::wave);
    g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    g.drawText ("drosaka voice", 16, 14, 300, 26, juce::Justification::centredLeft);
    g.setFont (juce::FontOptions (10.5f));
    g.setColour (colours::dim);
    g.drawText ("BANK", 150, 14, 42, 26, juce::Justification::centredRight);
    g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    g.setFont (juce::FontOptions (12.0f));
    g.setColour (colours::dim);
    g.drawText ("sort", sortPitch.getX() - 40, 14, 34, 26, juce::Justification::centredRight);

    if (dragHover)
    {
        g.setColour (colours::accent);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (2.0f), 6.0f, 2.0f);
    }
    const int n = shownBank != nullptr ? shownBank->size() : 0;

    // Section captions.
    auto grid = gridArea();
    g.setFont (juce::FontOptions (11.0f));
    g.setColour (colours::dim);
    g.drawText ("CONTROLS   right-click a knob to MIDI learn, double-click to reset", grid.getX(), grid.getY() - 20, grid.getWidth(), 16,
                juce::Justification::centredLeft);

    // Performance panel.
    auto panel = panelArea();
    g.setColour (colours::panel);
    g.fillRoundedRectangle (panel.toFloat(), 6.0f);
    g.setColour (colours::dim);
    g.drawText ("PERFORMANCE", panel.getX(), grid.getY() - 20, panel.getWidth(), 16, juce::Justification::centredLeft);
    {
        auto p = panel.reduced (10);
        g.setFont (juce::FontOptions (10.5f));
        g.setColour (colours::wave);
        g.drawText ("PLAY", playModeBox.getX(), p.getY(), 80, 14, juce::Justification::centredLeft);
        g.drawText ("PITCH LOCK", pitchLockBox.getX(), p.getY(), 90, 14, juce::Justification::centredLeft);
        g.drawText ("ENCODE STEPS", quantBox.getX(), p.getY(), 100, 14, juce::Justification::centredLeft);

        // Pattern status.
        const bool patternOn = playModeBox.getSelectedItemIndex() == 1;
        const int sl = proc.patternSlice.load();
        g.setColour (proc.patternActive.load() ? colours::slice : colours::dim);
        g.drawText (! patternOn ? "keys play slices directly"
                                : proc.patternActive.load() ? "playing slice " + juce::MidiMessage::getMidiNoteName (Bank::sliceRoot + juce::jmax (0, sl), true, true, 3)
                                                            : "hold a key: the loop starts from its slice on the next grid line",
                    p.getX(), p.getY() + 50, p.getWidth(), 14, juce::Justification::centredLeft);

        // Tune labels and the record meter.
        g.setFont (juce::FontOptions (10.5f));
        g.setColour (colours::wave);
        g.drawText ("ROOT", tuneRootBox.getX(), tuneRootBox.getY() - 16, 60, 14, juce::Justification::centredLeft);
        g.drawText ("SCALE", tuneScaleBox.getX(), tuneScaleBox.getY() - 16, 70, 14, juce::Justification::centredLeft);
        g.drawText ("TAKE", recordButton.getX(), recordButton.getY() - 16, 84, 14, juce::Justification::centredLeft);
        {
            const bool rec = proc.recordingNow.load();
            auto meter = juce::Rectangle<int> (recordButton.getX(), recordButton.getBottom() + 6, recordButton.getWidth(), 5);
            g.setColour (colours::row);
            g.fillRect (meter);
            g.setColour (juce::Colour (0xffe0303a));
            // Always live: shows whether Live is sending audio in, before you record.
            const float lvl = proc.inputLevel.load();
            g.setColour (rec ? juce::Colour (0xffe0303a) : colours::codec);
            g.fillRect (meter.withWidth (juce::roundToInt (juce::jmin (1.0f, lvl * 1.4f) * (float) meter.getWidth())));
            g.setColour (rec ? colours::wave : colours::dim);
            const int ins = proc.inputChannels.load();
            g.drawText (rec ? juce::String (proc.recordSeconds.load(), 1) + " s"
                            : ins == 0 ? juce::String ("no input")
                                       : lvl > 1e-4f ? juce::String ("input ") + juce::String (juce::roundToInt (juce::Decibels::gainToDecibels (lvl))) + " dB"
                                                     : juce::String ("input silent"),
                        meter.getX(), meter.getBottom() + 3, meter.getWidth() + 30, 14, juce::Justification::centredLeft);
        }

        // Stereo field: each audible voice as a dot (orange = primary, teal = encoded).
        auto f = fieldArea();
        g.setColour (colours::bg);
        g.fillRoundedRectangle (f.toFloat(), 4.0f);
        g.setColour (colours::row);
        g.drawVerticalLine (f.getCentreX(), (float) f.getY() + 2, (float) f.getBottom() - 2);
        g.setColour (colours::dim);
        g.setFont (juce::FontOptions (10.0f));
        g.drawText ("L", f.reduced (5, 0), juce::Justification::centredLeft);
        g.drawText ("R", f.reduced (5, 0), juce::Justification::centredRight);
        g.drawText (juce::String (proc.hostBpm.load(), 1) + " bpm  " + (proc.hostPlaying.load() ? "Live" : "internal clock"),
                    f.reduced (6, 3), juce::Justification::bottomLeft);
        if (shownBank != nullptr)
            for (int r = 0; r < n; ++r)
            {
                const int idx = shownBank->sorted[(size_t) r];
                const float lvl = proc.getVoiceLevel (idx);
                if (lvl <= 0.01f) continue;
                const float x = (float) f.getCentreX() + proc.getVoicePan (idx) * (float) (f.getWidth() / 2 - 16);
                const float y = (float) f.getBottom() - 18.0f - (float) (f.getHeight() - 30) * (n > 1 ? (float) r / (float) (n - 1) : 0.5f);
                const auto c = idx == shownBank->primary ? colours::accent
                                                         : colours::wave.interpolatedWith (colours::codec, proc.getVoiceEncode (idx));
                g.setColour (c.withAlpha (0.4f + 0.6f * juce::jmin (1.0f, lvl * 1.5f)));
                g.fillEllipse (x - 5.0f, y - 4.0f, 10.0f, 8.0f);
            }
    }

    // List header.
    g.setFont (juce::FontOptions (11.0f));
    g.setColour (colours::dim);
    g.drawText (juce::String (n) + (n == 1 ? " voice" : " voices") + juce::String (juce::CharPointer_UTF8 ("  low \xe2\x86\x92 high")),
                16, listView.getY() - 22, 140, 18, juce::Justification::centredLeft);
    {
        auto cols = VoiceRow::columns ({ listView.getX(), listView.getY() - 22, listView.getMaximumVisibleWidth(), 18 });
        g.drawText ("pitch       formant", cols.info.withTrimmedRight (8), juce::Justification::centredRight);
        g.drawText ("codec", cols.order, juce::Justification::centred);
        g.drawText ("alignment to primary  (drag, shift = fine, double-click = reset)", cols.lane, juce::Justification::centredLeft);
    }

    // Footer: errors, missing files, or how keys work.
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
        const int regions = proc.getSlices().numRegions();
        g.drawText (regions > 0 ? "C3 to " + juce::MidiMessage::getMidiNoteName (Bank::sliceRoot + regions - 1, true, true, 3)
                                    + " play the slices across every voice; any other key plays the whole phrase; hold C1-B1 to set the harmony"
                                : juce::String ("any note plays the whole bank in sync (C1-B1 set the harmony); double-click the waveform to slice"),
                    footer, juce::Justification::centredLeft);
    }
}
