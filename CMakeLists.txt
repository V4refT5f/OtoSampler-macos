cmake_minimum_required(VERSION 3.25)

project(OtoSampler VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT: the plugin has no dependency on the VC++ redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(OTO_COPY_AFTER_BUILD "Copy the VST3 to %CommonProgramFiles%/VST3 after build (needs admin rights)" OFF)
option(OTO_BUILD_STANDALONE "Also build a Standalone app (handy for testing without a DAW)" ON)
set(OTO_JUCE_TAG "8.0.8" CACHE STRING "JUCE git tag to fetch")

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        ${OTO_JUCE_TAG}
    GIT_SHALLOW    ON
)
FetchContent_MakeAvailable(JUCE)

set(OTO_FORMATS VST3)
if(OTO_BUILD_STANDALONE)
    list(APPEND OTO_FORMATS Standalone)
endif()

juce_add_plugin(OtoSampler
    COMPANY_NAME                "Oto Audio"
    PRODUCT_NAME                "Oto Sampler"
    BUNDLE_ID                   "com.otoaudio.otosampler"
    PLUGIN_MANUFACTURER_CODE    Otoa
    PLUGIN_CODE                 Osmp
    FORMATS                     ${OTO_FORMATS}
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    VST3_CATEGORIES             Instrument Sampler
    COPY_PLUGIN_AFTER_BUILD     ${OTO_COPY_AFTER_BUILD}
)

target_sources(OtoSampler PRIVATE
    Source/Common.h
    Source/Params.h
    Source/Settings.h
    Source/PluginProcessor.h
    Source/PluginProcessor.cpp
    Source/PluginEditor.h
    Source/PluginEditor.cpp
    Source/Dsp/SampleTypes.h
    Source/Dsp/Adsr.h
    Source/Dsp/Biquad.h
    Source/Dsp/PitchAnalyzer.h
    Source/Dsp/PitchAnalyzer.cpp
    Source/Dsp/Voice.h
    Source/Dsp/Voice.cpp
    Source/UI/Theme.h
    Source/UI/Widgets.h
    Source/UI/WaveformView.h
    Source/UI/Panels.h
)

target_include_directories(OtoSampler PRIVATE Source)

target_compile_definitions(OtoSampler PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_OGGVORBIS=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
)

target_compile_options(OtoSampler PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /MP>
)

target_link_libraries(OtoSampler
    PRIVATE
        juce::juce_audio_utils
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
