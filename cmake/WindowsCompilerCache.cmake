# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Aobus Contributors

# Visual Studio generators do not honor CMAKE_<LANG>_COMPILER_LAUNCHER.
# Validate a cl.exe-compatible wrapper before project() probes the compiler,
# then apply it only to compiled targets after the directory tree is complete.
set(AOBUS_MSBUILD_CL_TOOL_EXE "" CACHE FILEPATH
  "Optional cl.exe-compatible compiler-cache wrapper for Visual Studio generators")
set(AOBUS_MSBUILD_CL_TOOL_EXE_RESOLVED "")

if(AOBUS_MSBUILD_CL_TOOL_EXE)
  if(NOT CMAKE_GENERATOR MATCHES "^Visual Studio ")
    message(FATAL_ERROR
      "AOBUS_MSBUILD_CL_TOOL_EXE is supported only by Visual Studio generators")
  endif()
  if(NOT IS_ABSOLUTE "${AOBUS_MSBUILD_CL_TOOL_EXE}")
    message(FATAL_ERROR "AOBUS_MSBUILD_CL_TOOL_EXE must be an absolute path")
  endif()

  cmake_path(NORMAL_PATH AOBUS_MSBUILD_CL_TOOL_EXE
    OUTPUT_VARIABLE _aobus_msbuild_cl_tool_exe)
  cmake_path(GET _aobus_msbuild_cl_tool_exe FILENAME _aobus_msbuild_cl_tool_name)
  string(TOLOWER "${_aobus_msbuild_cl_tool_name}" _aobus_msbuild_cl_tool_name)

  if(NOT _aobus_msbuild_cl_tool_name STREQUAL "cl.exe")
    message(FATAL_ERROR
      "AOBUS_MSBUILD_CL_TOOL_EXE must name a cl.exe-compatible wrapper")
  endif()
  if(NOT EXISTS "${_aobus_msbuild_cl_tool_exe}")
    message(FATAL_ERROR
      "AOBUS_MSBUILD_CL_TOOL_EXE does not exist: ${_aobus_msbuild_cl_tool_exe}")
  endif()

  set(AOBUS_MSBUILD_CL_TOOL_EXE_RESOLVED "${_aobus_msbuild_cl_tool_exe}")
  set(AOBUS_MSBUILD_COMPILER_CACHE_PROPS
    "${CMAKE_BINARY_DIR}/AobusWindowsCompilerCache.props")
  configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/WindowsCompilerCache.props"
    "${AOBUS_MSBUILD_COMPILER_CACHE_PROPS}"
    COPYONLY)
  message(STATUS
    "Using MSBuild compiler cache wrapper for C/C++ targets: ${_aobus_msbuild_cl_tool_exe}")

  unset(_aobus_msbuild_cl_tool_exe)
  unset(_aobus_msbuild_cl_tool_name)
endif()

function(aobus_apply_msbuild_compiler_cache directory)
  if(NOT AOBUS_MSBUILD_CL_TOOL_EXE_RESOLVED)
    return()
  endif()

  # TrackFileAccess=false is required by sccache's MSBuild wrapper mode, but it
  # must be scoped to ClCompile. CMake-generated C++ projects also contain
  # custom rules that rely on file tracking to order generated output.
  set(_aobus_compiled_target_types
    EXECUTABLE
    MODULE_LIBRARY
    OBJECT_LIBRARY
    SHARED_LIBRARY
    STATIC_LIBRARY)

  get_property(_aobus_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(_aobus_target IN LISTS _aobus_targets)
    get_target_property(_aobus_target_type "${_aobus_target}" TYPE)
    if(_aobus_target_type IN_LIST _aobus_compiled_target_types)
      set_property(TARGET "${_aobus_target}" PROPERTY
        VS_GLOBAL_CLToolExe "${AOBUS_MSBUILD_CL_TOOL_EXE_RESOLVED}")
      set_property(TARGET "${_aobus_target}" APPEND PROPERTY VS_PROJECT_IMPORT
        "${AOBUS_MSBUILD_COMPILER_CACHE_PROPS}")
    endif()
  endforeach()

  get_property(_aobus_subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(_aobus_subdirectory IN LISTS _aobus_subdirectories)
    aobus_apply_msbuild_compiler_cache("${_aobus_subdirectory}")
  endforeach()
endfunction()
