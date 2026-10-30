cmake_minimum_required(VERSION 3.22)

project(DrosakaVoice VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE lives in ./JUCE (build.sh clones it on first run, pinned to a tag).
add_subdirectory(JUCE)

# AU + VST3 on macOS; VST3 only elsewhere (AU is Apple-only).
if(APPLE)
    set(DV_FORMATS AU VST3)
else()
    set(DV_FORMATS VST3)
endif()

juce_add_plugin(DrosakaVoice
    COMPANY_NAME                "Strange Frequencies"
    COMPANY_WEBSITE             "https://strangefrequencies.fm"
    BUNDLE_ID                   "fm.strangefrequencies.drosakavoice"
    PLUGIN_MANUFACTURER_CODE    Sfrq
    PLUGIN_CODE                 DrVc
    PRODUCT_NAME                "drosaka Voice"
    FORMATS                     ${DV_FORMATS}
    IS_SYNTH                    TRUE        # instrument: sits on a MIDI track
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES             Instrument Sampler
    AU_MAIN_TYPE                kAudioUnitType_MusicDevice
    COPY_PLUGIN_AFTER_BUILD     TRUE)       # copies into ~/Library/Audio/Plug-Ins/{Components,VST3}

target_sources(DrosakaVoice PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Analysis.cpp)

target_compile_definitions(DrosakaVoice PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(DrosakaVoice
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Offline processor tests:  cmake -B build -DDV_BUILD_TESTS=ON && cmake --build build --target DrosakaVoiceTests
option(DV_BUILD_TESTS "Build offline processor tests" OFF)
if(DV_BUILD_TESTS)
    juce_add_console_app(DrosakaVoiceTests PRODUCT_NAME "DrosakaVoiceTests")
    target_sources(DrosakaVoiceTests PRIVATE tests/ProcessorTests.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/Analysis.cpp)
    target_compile_definitions(DrosakaVoiceTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(DrosakaVoiceTests PRIVATE
        juce::juce_audio_utils juce::juce_audio_formats juce::juce_audio_processors juce::juce_dsp
        juce::juce_recommended_config_flags)
endif()
if(DV_BUILD_TESTS)
    juce_add_console_app(DrosakaVoiceSnapshot PRODUCT_NAME "DrosakaVoiceSnapshot")
    target_sources(DrosakaVoiceSnapshot PRIVATE tests/Snapshot.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/Analysis.cpp)
    target_compile_definitions(DrosakaVoiceSnapshot PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(DrosakaVoiceSnapshot PRIVATE
        juce::juce_audio_utils juce::juce_audio_formats juce::juce_audio_processors juce::juce_dsp
        juce::juce_recommended_config_flags)
endif()
