cmake_minimum_required(VERSION 3.25)
project(SubLab808 VERSION 1.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Apple Silicon build" FORCE)

include(FetchContent)
set(SUBLAB808_JUCE_PATH "" CACHE PATH "Optional path to a local JUCE checkout")
if (SUBLAB808_JUCE_PATH AND EXISTS "${SUBLAB808_JUCE_PATH}/CMakeLists.txt")
    add_subdirectory("${SUBLAB808_JUCE_PATH}" JUCE)
elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE/CMakeLists.txt")
    add_subdirectory(external/JUCE JUCE)
elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../ReverseLab/external/JUCE/CMakeLists.txt")
    add_subdirectory(../ReverseLab/external/JUCE JUCE)
else()
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.15
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_plugin(SubLab808
    COMPANY_NAME "Whykiki Audio"
    BUNDLE_ID "audio.whykiki.sublab808"
    PLUGIN_MANUFACTURER_CODE Wkik
    PLUGIN_CODE Sl08
    FORMATS VST3
    PRODUCT_NAME "SubLab808"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    VST3_CATEGORIES Instrument Synth
    COPY_PLUGIN_AFTER_BUILD FALSE)

juce_generate_juce_header(SubLab808)

target_sources(SubLab808 PRIVATE
    Source/PluginProcessor.cpp Source/PluginProcessor.h
    Source/PluginEditor.cpp Source/PluginEditor.h)
target_compile_definitions(SubLab808 PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(SubLab808 PRIVATE juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

juce_add_console_app(SubLab808Tests PRODUCT_NAME "SubLab808 Tests")
juce_generate_juce_header(SubLab808Tests)
target_sources(SubLab808Tests PRIVATE
    Tests/SmokeTest.cpp
    Source/PluginProcessor.cpp Source/PluginProcessor.h
    Source/PluginEditor.cpp Source/PluginEditor.h)
target_include_directories(SubLab808Tests PRIVATE Source)
target_compile_definitions(SubLab808Tests PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JucePlugin_Name="SubLab808")
target_link_libraries(SubLab808Tests PRIVATE juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

enable_testing()
add_test(NAME SubLab808Smoke COMMAND SubLab808Tests)
