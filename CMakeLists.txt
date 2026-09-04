cmake_minimum_required(VERSION 3.25)
if(CMAKE_HOST_APPLE)
    # Must be set before project(); Apple-only so the test suite still configures elsewhere.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Apple Silicon build" FORCE)
endif()
project(SubLab808 VERSION 1.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(SUBLAB808_JUCE_PATH "" CACHE PATH "Optional path to a local JUCE checkout")
set(SUBLAB808_JUCE_COMMIT "91ad83ae34a81e0833b1a2b0866f54846370ae53")
if (SUBLAB808_JUCE_PATH AND EXISTS "${SUBLAB808_JUCE_PATH}/CMakeLists.txt")
    execute_process(COMMAND git -C "${SUBLAB808_JUCE_PATH}" rev-parse HEAD
        OUTPUT_VARIABLE SUBLAB808_LOCAL_JUCE_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE)
    if (NOT SUBLAB808_LOCAL_JUCE_COMMIT STREQUAL SUBLAB808_JUCE_COMMIT)
        message(FATAL_ERROR "SUBLAB808_JUCE_PATH must point to JUCE commit ${SUBLAB808_JUCE_COMMIT}")
    endif()
    add_subdirectory("${SUBLAB808_JUCE_PATH}" JUCE)
else()
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG ${SUBLAB808_JUCE_COMMIT}
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
target_compile_definitions(SubLab808 PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
    SUBLAB808_VERSION_STRING="${PROJECT_VERSION}")
target_link_libraries(SubLab808 PRIVATE juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

if(APPLE)
    # JUCE generates moduleinfo.json after its initial ad-hoc signing step. Sign the completed
    # bundle last so the downloadable artifact has a valid resource seal.
    find_program(SUBLAB808_CODESIGN_EXECUTABLE codesign REQUIRED)
    set(SUBLAB808_CODESIGN_IDENTITY "-" CACHE STRING "Final VST3 signing identity; '-' selects ad-hoc signing")
    set(SUBLAB808_CODESIGN_OPTIONS --force --deep --sign "${SUBLAB808_CODESIGN_IDENTITY}")
    if(NOT SUBLAB808_CODESIGN_IDENTITY STREQUAL "-")
        list(APPEND SUBLAB808_CODESIGN_OPTIONS --options runtime --timestamp)
    endif()
    add_custom_command(TARGET SubLab808_VST3 POST_BUILD
        COMMAND "${SUBLAB808_CODESIGN_EXECUTABLE}" ${SUBLAB808_CODESIGN_OPTIONS} "$<TARGET_BUNDLE_DIR:SubLab808_VST3>"
        COMMENT "Signing completed SubLab808 VST3 bundle"
        VERBATIM)
endif()

juce_add_console_app(SubLab808Tests PRODUCT_NAME "SubLab808 Tests")
juce_generate_juce_header(SubLab808Tests)
target_sources(SubLab808Tests PRIVATE
    Tests/SmokeTest.cpp
    Source/PluginProcessor.cpp Source/PluginProcessor.h
    Source/PluginEditor.cpp Source/PluginEditor.h)
target_include_directories(SubLab808Tests PRIVATE Source)
target_compile_definitions(SubLab808Tests PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JucePlugin_Name="SubLab808"
    SUBLAB808_VERSION_STRING="${PROJECT_VERSION}")
target_link_libraries(SubLab808Tests PRIVATE juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

enable_testing()
add_test(NAME SubLab808Smoke COMMAND SubLab808Tests)

juce_add_console_app(SubLab808HostTests PRODUCT_NAME "SubLab808 Host Tests")
juce_generate_juce_header(SubLab808HostTests)
target_sources(SubLab808HostTests PRIVATE Tests/HostTest.cpp)
target_compile_definitions(SubLab808HostTests PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1)
target_link_libraries(SubLab808HostTests PRIVATE juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
add_dependencies(SubLab808HostTests SubLab808_VST3)
add_test(NAME SubLab808BundleLoad COMMAND SubLab808HostTests
    "${CMAKE_BINARY_DIR}/SubLab808_artefacts/$<CONFIG>/VST3/SubLab808.vst3")

# Preset persistence and complete factory-bank audio checks.
juce_add_console_app(SubLab808PresetTests PRODUCT_NAME "SubLab808 Preset Tests")
juce_generate_juce_header(SubLab808PresetTests)
target_sources(SubLab808PresetTests PRIVATE Tests/PresetTests.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_include_directories(SubLab808PresetTests PRIVATE Source)
target_compile_definitions(SubLab808PresetTests PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_MODAL_LOOPS_PERMITTED=1 PRESET_TEST_SUBLAB=1 JucePlugin_Name="SubLab808"
    SUBLAB808_VERSION_STRING="${PROJECT_VERSION}")
target_link_libraries(SubLab808PresetTests PRIVATE juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
add_test(NAME SubLab808Presets COMMAND SubLab808PresetTests)

if(APPLE)
    # Test-only Cocoa observation proves that native file panels really open and
    # close. Product code and non-macOS tests do not depend on this bridge.
    # Keep JUCE's existing CXX toolchain for .mm files so CMAKE_CXX_FLAGS
    # (including sanitizer instrumentation) still apply to every such source.
    target_sources(SubLab808PresetTests PRIVATE Tests/NativeFilePanel.mm)
endif()
