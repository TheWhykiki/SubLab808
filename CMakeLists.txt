cmake_minimum_required(VERSION 3.25)
if(CMAKE_HOST_APPLE)
    # Must be set before project(); Apple-only so the test suite still configures elsewhere.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal macOS build" FORCE)
endif()
project(SubLab808 VERSION 1.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    # The standalone VST3/MSI must also run on clean Windows installations that
    # do not already have a matching Visual C++ Redistributable installed.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

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
        GIT_TAG ${SUBLAB808_JUCE_COMMIT})
    FetchContent_MakeAvailable(JUCE)
endif()

if(WIN32)
    # JUCE names the two delivered ABIs x86_64 and arm64ec. Plain ARM64 uses a
    # different ABI/folder and cannot be loaded by the x64/ARM64EC hosts targeted here.
    set(SUBLAB808_SUPPORTED_WINDOWS_ARCHITECTURES x86_64 arm64ec)
    if(NOT JUCE_TARGET_ARCHITECTURE IN_LIST SUBLAB808_SUPPORTED_WINDOWS_ARCHITECTURES)
        message(FATAL_ERROR
            "Unsupported SubLab808 Windows architecture '${JUCE_TARGET_ARCHITECTURE}'; "
            "configure Visual Studio with -A x64 or -A ARM64EC")
    endif()

    # Reject a manually overridden or stale JUCE architecture cache instead of
    # putting one PE architecture into another architecture's VST3 folder.
    if(CMAKE_GENERATOR MATCHES "^Visual Studio ")
        if(JUCE_TARGET_ARCHITECTURE STREQUAL "x86_64")
            set(SUBLAB808_EXPECTED_VISUAL_STUDIO_PLATFORM x64)
        else()
            set(SUBLAB808_EXPECTED_VISUAL_STUDIO_PLATFORM ARM64EC)
        endif()
        if(NOT CMAKE_GENERATOR_PLATFORM STREQUAL SUBLAB808_EXPECTED_VISUAL_STUDIO_PLATFORM)
            message(FATAL_ERROR
                "Visual Studio platform '${CMAKE_GENERATOR_PLATFORM}' does not match JUCE's "
                "detected architecture '${JUCE_TARGET_ARCHITECTURE}'. Use a fresh build directory "
                "with -A ${SUBLAB808_EXPECTED_VISUAL_STUDIO_PLATFORM}.")
        endif()
    endif()

    # The pinned JUCE manifest helper must load the target VST3 during the build.
    # Require native Windows on Arm instead of misrepresenting a cross-build as validation.
    if(JUCE_TARGET_ARCHITECTURE STREQUAL "arm64ec")
        if(NOT CMAKE_GENERATOR MATCHES "^Visual Studio ")
            message(FATAL_ERROR "SubLab808 ARM64EC builds require a Visual Studio generator")
        endif()
        string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" SUBLAB808_WINDOWS_HOST_ARCHITECTURE)
        if(NOT SUBLAB808_WINDOWS_HOST_ARCHITECTURE MATCHES "^(arm64|arm64ec|aarch64)$"
           OR NOT JUCE_WINDOWS_HELPERS_CAN_RUN)
            message(FATAL_ERROR
                "SubLab808 ARM64EC builds require native Windows on Arm so the VST3 manifest "
                "helper and host test can run")
        endif()
    endif()
endif()

# JUCE 8.0.15 omits the Visual Studio platform from its nested VST3 manifest-helper
# configure. Keep the pinned workaround outside JUCE so ARM64EC builds generate metadata.
include(cmake/JuceVST3ManifestHelperPlatform.cmake)

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

include(cmake/Updater.cmake)
wk_add_updater(SubLab808)

if(APPLE)
    # The updater command above embeds the helper first. Sign nested code inside-out, then sign
    # the completed VST3 root so moduleinfo.json and the helper are covered by its resource seal.
    find_program(SUBLAB808_CODESIGN_EXECUTABLE codesign REQUIRED)
    set(SUBLAB808_CODESIGN_IDENTITY "-" CACHE STRING
        "Final VST3/updater signing identity; '-' selects ad-hoc signing")
    set(SUBLAB808_CODESIGN_OPTIONS --force --sign "${SUBLAB808_CODESIGN_IDENTITY}")
    if(NOT SUBLAB808_CODESIGN_IDENTITY STREQUAL "-")
        list(APPEND SUBLAB808_CODESIGN_OPTIONS --options runtime --timestamp)
    endif()
    add_custom_command(TARGET SubLab808_VST3 POST_BUILD
        COMMAND "${SUBLAB808_CODESIGN_EXECUTABLE}" ${SUBLAB808_CODESIGN_OPTIONS}
            "$<TARGET_BUNDLE_DIR:SubLab808_VST3>/Contents/Helpers/SubLab808Updater.app"
        COMMAND "${SUBLAB808_CODESIGN_EXECUTABLE}" ${SUBLAB808_CODESIGN_OPTIONS}
            "$<TARGET_BUNDLE_DIR:SubLab808_VST3>"
        COMMAND "${SUBLAB808_CODESIGN_EXECUTABLE}" --verify --deep --strict --verbose=2 "$<TARGET_BUNDLE_DIR:SubLab808_VST3>"
        COMMENT "Signing SubLab808 updater and completed VST3 bundle inside-out"
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
set_tests_properties(SubLab808Smoke PROPERTIES TIMEOUT 300)
# A dedicated invocation makes the real listener-lock regression fail promptly.
add_test(NAME SubLab808ListenerLock COMMAND SubLab808Tests --listener-lock-only)
set_tests_properties(SubLab808ListenerLock PROPERTIES TIMEOUT 20)

juce_add_console_app(SubLab808HostTests PRODUCT_NAME "SubLab808 Host Tests")
juce_generate_juce_header(SubLab808HostTests)
target_sources(SubLab808HostTests PRIVATE Tests/HostTest.cpp)
target_compile_definitions(SubLab808HostTests PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1)
target_link_libraries(SubLab808HostTests PRIVATE juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
add_dependencies(SubLab808HostTests SubLab808_VST3)
add_test(NAME SubLab808BundleLoad COMMAND SubLab808HostTests
    "$<GENEX_EVAL:$<TARGET_PROPERTY:SubLab808_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>")
set_tests_properties(SubLab808BundleLoad PROPERTIES TIMEOUT 300)

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
set_tests_properties(SubLab808Presets PROPERTIES TIMEOUT 600)

if(APPLE)
    # Test-only Cocoa observation proves that native file panels really open and
    # close. Product code and non-macOS tests do not depend on this bridge.
    # Keep JUCE's existing CXX toolchain for .mm files so CMAKE_CXX_FLAGS
    # (including sanitizer instrumentation) still apply to every such source.
    target_sources(SubLab808PresetTests PRIVATE Tests/NativeFilePanel.mm)
    # A fresh process per transition gives each real panel a tight,
    # independently reported failure. The sequential test below separately
    # proves that no AppKit modal state leaks from one case into the next.
    foreach(native_operation IN ITEMS import export)
        foreach(native_action IN ITEMS ancestor-hide detach destroy hide-then-destroy)
            set(native_case "${native_operation}-${native_action}")
            string(REPLACE "-" "_" native_case_id "${native_case}")
            set(native_test "SubLab808NativeChooser_${native_case_id}")
            add_test(NAME "${native_test}" COMMAND "${CMAKE_COMMAND}" -E env
                "WHYKIKI_PRESET_TEST_NATIVE_ONLY=1"
                "WHYKIKI_PRESET_TEST_NATIVE_CASE=${native_case}"
                "$<TARGET_FILE:SubLab808PresetTests>")
            set_tests_properties("${native_test}" PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
        endforeach()
    endforeach()
    # Also run every transition in one process. This catches a native modal
    # session that appears closed but poisons the next chooser; isolated cases
    # alone cannot detect that lifecycle leak.
    add_test(NAME SubLab808NativeChoosersSequential COMMAND "${CMAKE_COMMAND}" -E env
        --unset=WHYKIKI_PRESET_TEST_NATIVE_CASE
        "WHYKIKI_PRESET_TEST_NATIVE_ONLY=1"
        "$<TARGET_FILE:SubLab808PresetTests>")
    set_tests_properties(SubLab808NativeChoosersSequential PROPERTIES TIMEOUT 480 RUN_SERIAL TRUE)
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME SubLab808ReleasePipeline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
    -s "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests" -p "test_release_pipeline.py")
add_test(NAME SubLab808WindowsUpdaterContract COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
    -s "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests" -p "test_windows_updater.py")
add_test(NAME SubLab808WindowsInstallerContract COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
    -s "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests" -p "test_windows_installer_contract.py")
add_test(NAME SubLab808WindowsIntegrationContract COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
    -s "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests" -p "test_windows_integration.py")
add_test(NAME SubLab808WindowsReleaseContract COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
    -s "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests" -p "test_windows_release_contract.py")
