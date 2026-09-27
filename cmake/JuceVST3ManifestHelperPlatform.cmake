# JUCE 8.0.15 does not forward the outer Visual Studio platform to the
# nested VST3 manifest-helper configure. That makes a native ARM64EC plug-in
# build create a plain ARM64 helper, which cannot load the ARM64EC plug-in.
#
# Keep this override deliberately narrow and pinned to the reviewed JUCE
# implementation. A JUCE update must remove this file or review the copied
# function against the new upstream implementation.
include_guard(GLOBAL)

if(NOT WIN32 OR NOT CMAKE_GENERATOR MATCHES "^Visual Studio ")
    return()
endif()

get_directory_property(_juce_workaround_version
    DIRECTORY "${JUCE_SOURCE_DIR}" DEFINITION JUCE_VERSION)

if(NOT _juce_workaround_version STREQUAL "8.0.15")
    message(FATAL_ERROR
        "This VST3 helper platform override was reviewed only for JUCE 8.0.15; "
        "found '${_juce_workaround_version}'. Review it against JUCEUtils.cmake before building.")
endif()

if(NOT COMMAND _juce_add_vst3_manifest_helper_target)
    message(FATAL_ERROR
        "JUCE's _juce_add_vst3_manifest_helper_target command is unavailable; "
        "the Visual Studio platform override cannot be applied.")
endif()

if(NOT (CMAKE_GENERATOR_PLATFORM STREQUAL "x64"
        OR CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64EC"))
    message(FATAL_ERROR
        "VST3 builds require an explicit supported Visual Studio platform: "
        "configure with -A x64 or -A ARM64EC")
endif()

# This is JUCE 8.0.15's private helper function with one intentional change:
# -A${CMAKE_GENERATOR_PLATFORM} is forwarded to its nested CMake configure.
function(_juce_add_vst3_manifest_helper_target shared_code_target out_target out_executable_path)
    set(helper_target ${shared_code_target}_vst3_helper)

    if(TARGET ${helper_target}
       OR (CMAKE_SYSTEM_NAME STREQUAL "iOS")
       OR (CMAKE_SYSTEM_NAME STREQUAL "Android")
       OR (CMAKE_SYSTEM_NAME MATCHES ".*BSD"))
        return()
    endif()

    get_target_property(juce_library_code "${shared_code_target}" JUCE_GENERATED_SOURCES_DIRECTORY)
    set(build_dir "${CMAKE_BINARY_DIR}/vst3_helpers/${shared_code_target}")
    set(helper_name "vst3_helper")

    set(shared_defs_file "${build_dir}/shared_defs_$<CONFIG>.txt")
    file(GENERATE OUTPUT "${shared_defs_file}" CONTENT "$<TARGET_PROPERTY:${shared_code_target},COMPILE_DEFINITIONS>")

    set(shared_incs_file "${build_dir}/shared_incs_$<CONFIG>.txt")
    file(GENERATE OUTPUT "${shared_incs_file}" CONTENT "$<TARGET_PROPERTY:${shared_code_target},INCLUDE_DIRECTORIES>")

    set(PASSTHROUGH_ARGS "")

    if(CMAKE_CXX_COMPILER)
        list(APPEND PASSTHROUGH_ARGS "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
    endif()

    if(CMAKE_RC_COMPILER)
        list(APPEND PASSTHROUGH_ARGS "-DCMAKE_RC_COMPILER=${CMAKE_RC_COMPILER}")
    endif()

    list(APPEND PASSTHROUGH_ARGS "-A${CMAKE_GENERATOR_PLATFORM}")

    add_custom_target(${helper_target}
        COMMAND "${CMAKE_COMMAND}"
            "-G${CMAKE_GENERATOR}"
            "-S${JUCE_CMAKE_UTILS_DIR}/juce_vst3_helper"
            "-B${build_dir}"
            "-Dhelper_name=${helper_name}"
            "-Dsource_file=$<TARGET_PROPERTY:juce_audio_plugin_client,INTERFACE_JUCE_MODULE_PATH>/juce_audio_plugin_client/VST3/juce_VST3ManifestHelper.cpp"
            "-Dshared_defs_file=${shared_defs_file}"
            "-Dshared_incs_file=${shared_incs_file}"
            ${PASSTHROUGH_ARGS}

        COMMAND "${CMAKE_COMMAND}" --build "${build_dir}"

        COMMENT "Building VST3 manifest helper for ${shared_code_target}"
        VERBATIM)

    set(${out_executable_path} "${build_dir}/${helper_name}${CMAKE_EXECUTABLE_SUFFIX}" PARENT_SCOPE)
    set(${out_target} ${helper_target} PARENT_SCOPE)
endfunction()
