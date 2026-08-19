# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Aobus Contributors

function(aobus_verify_windows_utf8_manifest target)
  if(NOT WIN32)
    return()
  endif()

  if(NOT MSVC)
    message(FATAL_ERROR "The Aobus Windows UTF-8 manifest requires the MSVC linker")
  endif()

  if(NOT CMAKE_MT)
    message(FATAL_ERROR "The Windows SDK manifest tool is required")
  endif()

  set(verifier "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/VerifyWindowsUtf8Manifest.cmake")
  set(extracted_manifest "$<TARGET_FILE_DIR:${target}>/${target}.embedded.manifest")

  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
            "-DMT_EXECUTABLE=${CMAKE_MT}"
            "-DEXECUTABLE=$<TARGET_FILE:${target}>"
            "-DOUTPUT=${extracted_manifest}"
            -P "${verifier}"
    COMMENT "Verifying UTF-8 process manifest in ${target}"
    VERBATIM)
  set_property(TARGET ${target} PROPERTY AOBUS_WINDOWS_UTF8_MANIFEST_VERIFIED TRUE)
endfunction()

function(aobus_enable_windows_utf8_manifest target)
  if(NOT WIN32)
    return()
  endif()

  set(manifest "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../app/windows/utf8-process.manifest")
  target_sources(${target} PRIVATE "${manifest}")
  set_source_files_properties("${manifest}" PROPERTIES VS_TOOL_OVERRIDE Manifest)
  aobus_verify_windows_utf8_manifest(${target})
endfunction()

function(aobus_verify_windows_utf8_manifest_coverage directory)
  if(NOT WIN32)
    return()
  endif()

  get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target IN LISTS targets)
    get_target_property(type ${target} TYPE)
    get_target_property(source_dir ${target} SOURCE_DIR)
    cmake_path(IS_PREFIX CMAKE_SOURCE_DIR "${source_dir}" NORMALIZE is_first_party)

    if(type STREQUAL "EXECUTABLE" AND is_first_party)
      get_target_property(verified ${target} AOBUS_WINDOWS_UTF8_MANIFEST_VERIFIED)
      if(NOT verified)
        message(FATAL_ERROR
          "First-party Windows executable '${target}' has no verified UTF-8 process manifest")
      endif()
    endif()
  endforeach()

  get_property(subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(subdirectory IN LISTS subdirectories)
    cmake_path(IS_PREFIX CMAKE_SOURCE_DIR "${subdirectory}" NORMALIZE is_first_party)
    if(is_first_party)
      aobus_verify_windows_utf8_manifest_coverage("${subdirectory}")
    endif()
  endforeach()
endfunction()
