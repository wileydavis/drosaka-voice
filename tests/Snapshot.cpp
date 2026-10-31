#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
#include <cstdio>
#include <cstdlib>
int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    DrosakaVoiceProcessor p; p.prepareToPlay (48000, 256);
    juce::StringArray paths, errs;
    for (int i = 1; i < argc; ++i) paths.add (argv[i]);
    p.addStems (paths, errs);
    if (std::getenv ("DV_SNAP_DEMO")) { p.setPrimary (2); p.setMute (4, true); }
    p.params.getParameter ("count")->setValueNotifyingHost (0.55f);
    p.params.getParameter ("encodeCount")->setValueNotifyingHost (0.5f);
    p.params.getParameter ("bitRate")->setValueNotifyingHost (0.6f);
    p.params.getParameter ("spread")->setValueNotifyingHost (0.6f);
    juce::AudioBuffer<float> b (2, 256); juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
    for (int i = 0; i < 60; ++i) { p.processBlock (b, m); m.clear(); }
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    for (int i = 0; i < 5; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil (40);
    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
    juce::File out (std::getenv ("DV_SNAP_OUT") ? std::getenv ("DV_SNAP_OUT") : "/home/claude/ui.png"); out.deleteFile();
    juce::FileOutputStream os (out); juce::PNGImageFormat().writeImageToStream (img, os);
    std::printf ("errors: %s\n", errs.joinIntoString (", ").toRawUTF8());
}
