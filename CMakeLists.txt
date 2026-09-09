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

if(WIN32)
    set(SUBLAB808_WINDOWS_RELEASE_GATE_PUBLIC_KEY_XY "" CACHE STRING
        "P-256 release-gate public key as 128 uppercase hex characters (X then Y)")
    set(SUBLAB808_WINDOWS_RELEASE_GATE_NEXT_PUBLIC_KEY_XY "" CACHE STRING
        "Optional next P-256 release-gate public key as 128 uppercase hex characters (X then Y)")
endif()

include(cmake/Updater.cmake)
wk_add_updater(SubLab808)

if(WIN32)
    # These well-known points (private scalars 1 and 2) are used only by the
    # non-distribution self-test/compile-shape binaries. Production must always
    # receive separately managed public keys through the cache entries above.
    set(sublab808_test_release_gate_public_key_xy
        "6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C2964FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5")
    set(sublab808_test_release_gate_next_public_key_xy
        "7CF27B188D034F7E8A52380304B51AC3C08969E277F21B35A60B48FC4766997807775510DB8ED040293D9AC69F7430DBBA7DADE63CE982299E04B79D227873D1")
    foreach(updater_test_target IN ITEMS
            SubLab808WindowsUpdaterSelfTests
            SubLab808WindowsUpdaterProductionShape)
        if(TARGET ${updater_test_target})
            target_compile_definitions(${updater_test_target} PRIVATE
                WK_WINDOWS_UPDATER_RELEASE_GATE_PUBLIC_KEY_XY="${sublab808_test_release_gate_public_key_xy}"
                WK_WINDOWS_UPDATER_RELEASE_GATE_NEXT_PUBLIC_KEY_XY="${sublab808_test_release_gate_next_public_key_xy}")
        endif()
    endforeach()

    if(TARGET SubLab808WindowsUpdater)
        string(LENGTH "${SUBLAB808_WINDOWS_RELEASE_GATE_PUBLIC_KEY_XY}"
            sublab808_release_gate_public_key_length)
        if(NOT sublab808_release_gate_public_key_length EQUAL 128 OR
           NOT SUBLAB808_WINDOWS_RELEASE_GATE_PUBLIC_KEY_XY MATCHES "^[0-9A-F]+$")
            message(FATAL_ERROR
                "SUBLAB808_WINDOWS_RELEASE_GATE_PUBLIC_KEY_XY must be exactly 128 uppercase hexadecimal characters (P-256 X then Y)")
        endif()
        set(sublab808_release_gate_next_public_key_xy
            "${SUBLAB808_WINDOWS_RELEASE_GATE_NEXT_PUBLIC_KEY_XY}")
        if(NOT sublab808_release_gate_next_public_key_xy STREQUAL "")
            string(LENGTH "${sublab808_release_gate_next_public_key_xy}"
                sublab808_release_gate_next_public_key_length)
            if(NOT sublab808_release_gate_next_public_key_length EQUAL 128 OR
               NOT sublab808_release_gate_next_public_key_xy MATCHES "^[0-9A-F]+$")
                message(FATAL_ERROR
                    "SUBLAB808_WINDOWS_RELEASE_GATE_NEXT_PUBLIC_KEY_XY must be empty or exactly 128 uppercase hexadecimal characters (P-256 X then Y)")
            endif()
            if(sublab808_release_gate_next_public_key_xy STREQUAL
               SUBLAB808_WINDOWS_RELEASE_GATE_PUBLIC_KEY_XY)
                message(FATAL_ERROR
                    "SUBLAB808_WINDOWS_RELEASE_GATE_NEXT_PUBLIC_KEY_XY must differ from SUBLAB808_WINDOWS_RELEASE_GATE_PUBLIC_KEY_XY")
            endif()
        endif()
        target_compile_definitions(SubLab808WindowsUpdater PRIVATE
            WK_WINDOWS_UPDATER_RELEASE_GATE_PUBLIC_KEY_XY="${SUBLAB808_WINDOWS_RELEASE_GATE_PUBLIC_KEY_XY}"
            WK_WINDOWS_UPDATER_RELEASE_GATE_NEXT_PUBLIC_KEY_XY="${sublab808_release_gate_next_public_key_xy}")
    endif()
endif()

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
    # Every macOS binary that instantiates PresetBar gets the same module-local
    # native-callback lifetime guard. Keep it independent of updater packaging.
    foreach(module_pin_target IN ITEMS SubLab808 SubLab808Tests SubLab808PresetTests)
        target_sources(${module_pin_target} PRIVATE Source/MacModulePin.cpp Source/MacModulePin.h)
    endforeach()

    # Prove the guard with a genuinely unloadable module. Sanitizer runtimes can
    # make an instrumented DSO non-unloadable themselves, invalidating the
    # negative control; the normal arm64/x86_64 jobs run these two probes.
    set(SUBLAB808_MODULE_PIN_TEST_FLAGS
        "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS} ${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_MODULE_LINKER_FLAGS}")
    if(NOT SUBLAB808_MODULE_PIN_TEST_FLAGS MATCHES "(^|[ ;])-fsanitize=")
        add_library(SubLab808MacModulePinFixture MODULE
            Tests/MacModulePinFixture.cpp Source/MacModulePin.cpp Source/MacModulePin.h)
        target_include_directories(SubLab808MacModulePinFixture PRIVATE Source)
        target_link_libraries(SubLab808MacModulePinFixture PRIVATE juce::juce_recommended_warning_flags)
        add_executable(SubLab808MacModulePinTests Tests/MacModulePinTests.cpp)
        target_link_libraries(SubLab808MacModulePinTests PRIVATE juce::juce_recommended_warning_flags)
        add_dependencies(SubLab808MacModulePinTests SubLab808MacModulePinFixture)
        add_dependencies(SubLab808PresetTests SubLab808MacModulePinTests)
        add_test(NAME SubLab808MacModulePinPinned COMMAND SubLab808MacModulePinTests
            pinned "$<TARGET_FILE:SubLab808MacModulePinFixture>")
        add_test(NAME SubLab808MacModulePinUnpinned COMMAND SubLab808MacModulePinTests
            unpinned "$<TARGET_FILE:SubLab808MacModulePinFixture>")
        set_tests_properties(SubLab808MacModulePinPinned SubLab808MacModulePinUnpinned
            PROPERTIES TIMEOUT 30)
    endif()
    unset(SUBLAB808_MODULE_PIN_TEST_FLAGS)

    # Test-only Cocoa observation proves that native file panels really open and
    # close. Product code and non-macOS tests do not depend on this bridge.
    # Updater.cmake enables Objective-C++; sanitizer configurations must pass
    # matching CMAKE_OBJCXX_FLAGS so this lifecycle bridge is instrumented too.
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
add_test(NAME SubLab808WindowsReleaseBootstrap COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
    -s "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests" -p "test_windows_release_bootstrap.py")
add_test(NAME SubLab808WindowsReleaseState COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
    -s "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests" -p "test_windows_release_state.py")
