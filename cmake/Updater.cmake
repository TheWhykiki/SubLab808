# Native helper only: plugin audio callbacks never perform networking or installation.
if(APPLE)
    enable_language(OBJCXX)
endif()

function(wk_add_updater product)
    # The release/URL/MSI policy is deliberately portable and is compiled on
    # every platform, independently of the native updater UI.
    add_executable(${product}WindowsUpdaterPolicyTests
        Tests/WindowsUpdater/PolicyTests.cpp
        Updater/Windows/UpdaterPolicy.cpp)
    target_include_directories(${product}WindowsUpdaterPolicyTests PRIVATE Updater/Windows)
    target_compile_features(${product}WindowsUpdaterPolicyTests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(${product}WindowsUpdaterPolicyTests PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${product}WindowsUpdaterPolicyTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME ${product}WindowsUpdaterPolicy COMMAND ${product}WindowsUpdaterPolicyTests)
    set_tests_properties(${product}WindowsUpdaterPolicy PROPERTIES TIMEOUT 20)

    if(APPLE)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        set(updater_app "${CMAKE_CURRENT_BINARY_DIR}/Updater/${product}Updater.app")
        set(updater_arch_args "")
        foreach(architecture IN LISTS CMAKE_OSX_ARCHITECTURES)
            list(APPEND updater_arch_args --arch "${architecture}")
        endforeach()
        add_custom_command(OUTPUT "${updater_app}/Contents/MacOS/${product}Updater"
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build-updater.py"
                --product "${product}" --version "${PROJECT_VERSION}" --output "${updater_app}" ${updater_arch_args}
            DEPENDS scripts/build-updater.py Updater/main.swift Updater/UpdateCore.swift
                    Updater/HTTPClient.swift Updater/PackageService.swift
                    Updater/UpdaterApp.swift Updater/InstallationRecord.swift
            VERBATIM)
        add_custom_target(${product}Updater DEPENDS "${updater_app}/Contents/MacOS/${product}Updater")
        add_test(NAME ${product}UpdaterPolicy COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test-updater.py")
        add_dependencies(${product}_VST3 ${product}Updater)
        set_property(TARGET ${product}_VST3 APPEND PROPERTY LINK_DEPENDS "${updater_app}/Contents/MacOS/${product}Updater")
        target_sources(${product} PRIVATE Source/UpdaterLauncher.mm)
        target_compile_definitions(${product} PRIVATE WK_UPDATER_ENABLED=1)
        add_custom_command(TARGET ${product}_VST3 POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${updater_app}"
                "$<TARGET_BUNDLE_DIR:${product}_VST3>/Contents/Helpers/${product}Updater.app"
            COMMENT "Embedding ${product} native updater before final bundle signing"
            VERBATIM)
        return()
    endif()

    if(NOT WIN32)
        return()
    endif()

    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/Installer/Windows/package-config.json" installer_config)
    string(JSON configured_product GET "${installer_config}" productName)
    string(JSON configured_manufacturer GET "${installer_config}" manufacturer)
    if(NOT configured_product STREQUAL product OR NOT configured_manufacturer STREQUAL "Whykiki Audio")
        message(FATAL_ERROR "Windows installer identity does not match ${product}/Whykiki Audio")
    endif()

    if(JUCE_TARGET_ARCHITECTURE STREQUAL "x86_64")
        set(updater_architecture x64)
        set(other_updater_architecture arm64ec)
    elseif(JUCE_TARGET_ARCHITECTURE STREQUAL "arm64ec")
        set(updater_architecture arm64ec)
        set(other_updater_architecture x64)
    else()
        message(FATAL_ERROR "${product} updater supports only Windows x64 and ARM64EC")
    endif()
    string(JSON updater_upgrade_code GET "${installer_config}" upgradeCodes "${updater_architecture}")
    string(JSON updater_other_upgrade_code GET "${installer_config}" upgradeCodes "${other_updater_architecture}")

    set(WK_WINDOWS_UPDATER_RESOURCE_PRODUCT "${product}")
    set(WK_WINDOWS_UPDATER_RESOURCE_MANUFACTURER "${configured_manufacturer}")
    set(WK_WINDOWS_UPDATER_RESOURCE_VERSION "${PROJECT_VERSION}")
    set(WK_WINDOWS_UPDATER_RESOURCE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
    set(WK_WINDOWS_UPDATER_RESOURCE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
    set(WK_WINDOWS_UPDATER_RESOURCE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Updater")
    set(updater_version_resource "${CMAKE_CURRENT_BINARY_DIR}/Updater/${product}UpdaterVersion.rc")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Updater/Windows/UpdaterVersion.rc.in"
                   "${updater_version_resource}" @ONLY)

    set(updater_sources
        Updater/Windows/WindowsUpdater.cpp
        Updater/Windows/UpdaterPolicy.cpp)
    set(updater_definitions
        WK_WINDOWS_UPDATER_PRODUCT="${product}"
        WK_WINDOWS_UPDATER_VERSION="${PROJECT_VERSION}"
        WK_WINDOWS_UPDATER_MANUFACTURER="${configured_manufacturer}"
        WK_WINDOWS_UPDATER_GITHUB_OWNER="TheWhykiki"
        WK_WINDOWS_UPDATER_GITHUB_REPOSITORY="${product}"
        WK_WINDOWS_UPDATER_UPGRADE_CODE="${updater_upgrade_code}"
        WK_WINDOWS_UPDATER_OTHER_UPGRADE_CODE="${updater_other_upgrade_code}"
        UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    set(updater_libraries juce::juce_core bcrypt comctl32 crypt32 msi ole32 shell32 winhttp wintrust advapi32)

    # This target compiles the complete native implementation but is prevented
    # in source from opening files, networking, elevating or installing.
    add_executable(${product}WindowsUpdaterSelfTests
        Tests/WindowsUpdater/WindowsUpdaterTests.cpp
        Updater/Windows/Updater.manifest ${updater_sources})
    target_include_directories(${product}WindowsUpdaterSelfTests PRIVATE Updater/Windows)
    target_compile_definitions(${product}WindowsUpdaterSelfTests PRIVATE
        ${updater_definitions} WK_WINDOWS_UPDATER_TEST_MODE=1)
    target_link_libraries(${product}WindowsUpdaterSelfTests PRIVATE ${updater_libraries}
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    target_compile_options(${product}WindowsUpdaterSelfTests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME ${product}WindowsUpdaterSelfTest COMMAND ${product}WindowsUpdaterSelfTests)
    set_tests_properties(${product}WindowsUpdaterSelfTest PROPERTIES TIMEOUT 30)

    # Compile the exact production entry point, subsystem and manifest in normal
    # CI even when release credentials are unavailable. This unmistakably named
    # dummy-pin binary is never embedded, packaged, uploaded or executed.
    set(non_distribution_signer "1111111111111111111111111111111111111111111111111111111111111111")
    add_executable(${product}WindowsUpdaterProductionShape WIN32
        Updater/Windows/main.cpp Updater/Windows/Updater.manifest
        "${updater_version_resource}" ${updater_sources})
    target_include_directories(${product}WindowsUpdaterProductionShape PRIVATE Updater/Windows)
    target_compile_definitions(${product}WindowsUpdaterProductionShape PRIVATE
        ${updater_definitions} WK_WINDOWS_UPDATER_SIGNER_SHA256="${non_distribution_signer}"
        WK_WINDOWS_UPDATER_COMPILE_ONLY=1)
    target_link_libraries(${product}WindowsUpdaterProductionShape PRIVATE ${updater_libraries}
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    target_compile_options(${product}WindowsUpdaterProductionShape PRIVATE /W4 /permissive- /utf-8)
    set_target_properties(${product}WindowsUpdaterProductionShape PROPERTIES
        OUTPUT_NAME "${product}Updater-UNSIGNED-NOT-FOR-DISTRIBUTION")

    # An OBJECT library proves only compilation. Keep a separate, explicitly
    # requested non-distribution executable so Windows CI also resolves the
    # launcher's JUCE, WinTrust and Crypt32 symbols through the real linker.
    # Its consumer deliberately supplies an invalid product name, making an
    # accidental execution fail before filesystem, trust or process work.
    add_executable(${product}WindowsUpdaterLauncherShape EXCLUDE_FROM_ALL
        Tests/WindowsUpdater/LauncherLinkShape.cpp
        Source/UpdaterLauncher.cpp)
    target_compile_definitions(${product}WindowsUpdaterLauncherShape PRIVATE
        WK_UPDATER_ENABLED=1 WK_WINDOWS_UPDATER_SIGNER_SHA256="${non_distribution_signer}")
    target_link_libraries(${product}WindowsUpdaterLauncherShape PRIVATE juce::juce_gui_basics
        crypt32 wintrust
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    target_compile_options(${product}WindowsUpdaterLauncherShape PRIVATE /W4 /permissive- /utf-8)
    set_target_properties(${product}WindowsUpdaterLauncherShape PROPERTIES
        OUTPUT_NAME "${product}UpdaterLauncherLinkShape-UNSIGNED-NOT-FOR-DISTRIBUTION")

    string(TOUPPER "${product}" product_upper)
    set(signer_variable "${product_upper}_WINDOWS_UPDATER_SIGNER_SHA256")
    set(${signer_variable} "" CACHE STRING
        "SHA-256 distribution certificate fingerprint that enables the signed Windows updater")
    set(updater_signer "${${signer_variable}}")
    if(updater_signer STREQUAL "")
        message(STATUS "${product}: Windows updater UI disabled until ${signer_variable} is configured")
        return()
    endif()
    string(LENGTH "${updater_signer}" updater_signer_length)
    if(NOT updater_signer_length EQUAL 64 OR NOT updater_signer MATCHES "^[0-9A-Fa-f]+$")
        message(FATAL_ERROR "${signer_variable} must be exactly 64 hexadecimal characters")
    endif()

    add_executable(${product}WindowsUpdater WIN32
        Updater/Windows/main.cpp Updater/Windows/Updater.manifest
        "${updater_version_resource}" ${updater_sources})
    target_include_directories(${product}WindowsUpdater PRIVATE Updater/Windows)
    target_compile_definitions(${product}WindowsUpdater PRIVATE
        ${updater_definitions} WK_WINDOWS_UPDATER_SIGNER_SHA256="${updater_signer}")
    target_link_libraries(${product}WindowsUpdater PRIVATE ${updater_libraries}
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    target_compile_options(${product}WindowsUpdater PRIVATE /W4 /permissive- /utf-8)
    set_target_properties(${product}WindowsUpdater PROPERTIES OUTPUT_NAME "${product}Updater")

    add_dependencies(${product}_VST3 ${product}WindowsUpdater)
    set_property(TARGET ${product}_VST3 APPEND PROPERTY LINK_DEPENDS "$<TARGET_FILE:${product}WindowsUpdater>")
    target_sources(${product} PRIVATE Source/UpdaterLauncher.cpp)
    target_compile_definitions(${product} PRIVATE
        WK_UPDATER_ENABLED=1 WK_WINDOWS_UPDATER_SIGNER_SHA256="${updater_signer}")
    target_link_libraries(${product} PRIVATE crypt32 wintrust)
    set(vst3_bundle "$<GENEX_EVAL:$<TARGET_PROPERTY:${product}_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>")
    add_custom_command(TARGET ${product}_VST3 POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${vst3_bundle}/Contents/Helpers"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${product}WindowsUpdater>"
            "${vst3_bundle}/Contents/Helpers/${product}Updater.exe"
        COMMENT "Embedding ${product} Windows updater before Authenticode/MSI signing"
        VERBATIM)
endfunction()
