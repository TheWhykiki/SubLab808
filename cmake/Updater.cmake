# Native helper only: plugin audio callbacks never perform networking or installation.
if(APPLE)
    enable_language(OBJCXX)
endif()
function(wk_add_updater product)
    if(NOT APPLE)
        return()
    endif()
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
endfunction()
