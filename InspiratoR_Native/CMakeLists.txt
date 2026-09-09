cmake_minimum_required(VERSION 3.25)
project(InspiratoR VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    add_subdirectory(JUCE)
else()
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 9.0.2
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(JUCE)
endif()

if (APPLE)
    set(INSPIRATOR_FORMATS VST3 AU)
else()
    set(INSPIRATOR_FORMATS VST3)
endif()

juce_add_plugin(InspiratoR
    COMPANY_NAME "InspiratoR"
    BUNDLE_ID "com.inspirator.native"
    PLUGIN_MANUFACTURER_CODE Nilo
    PLUGIN_CODE Insp
    PRODUCT_NAME "InspiratoR"
    PLUGIN_NAME "InspiratoR"
    DESCRIPTION "Generative melody and chord MIDI sequencer"
    VERSION 0.2.0

    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    IS_MIDI_EFFECT TRUE
    VST_NUM_MIDI_INS 1
    VST_NUM_MIDI_OUTS 1
    VST3_CATEGORIES Fx Generator Tools
    AU_MAIN_TYPE kAudioUnitType_MIDIProcessor
    AU_SANDBOX_SAFE TRUE

    FORMATS ${INSPIRATOR_FORMATS}
    COPY_PLUGIN_AFTER_BUILD FALSE
)

juce_generate_juce_header(InspiratoR)

target_sources(InspiratoR PRIVATE
    Source/InspiratoEngine.cpp
    Source/InspiratoEngine.h
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/InspiratoLookAndFeel.cpp
    Source/InspiratoLookAndFeel.h
)

target_compile_definitions(InspiratoR PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
)

target_link_libraries(InspiratoR PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

add_executable(InspiratoEngineSmoke
    Tests/EngineSmoke.cpp
    Source/InspiratoEngine.cpp
)
target_include_directories(InspiratoEngineSmoke PRIVATE Source)
target_compile_features(InspiratoEngineSmoke PRIVATE cxx_std_20)

enable_testing()
add_test(NAME InspiratoEngineSmoke COMMAND InspiratoEngineSmoke)
