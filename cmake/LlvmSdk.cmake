# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2026 Aobus Contributors

include_guard(GLOBAL)

set(AOBUS_LLVM_SDK_VERSION "22.1.8")
set(AOBUS_LLVM_SDK_ARCHIVE
  "clang+llvm-${AOBUS_LLVM_SDK_VERSION}-x86_64-pc-windows-msvc.tar.xz")
set(AOBUS_LLVM_SDK_URL
  "https://github.com/llvm/llvm-project/releases/download/llvmorg-${AOBUS_LLVM_SDK_VERSION}/clang%2Bllvm-${AOBUS_LLVM_SDK_VERSION}-x86_64-pc-windows-msvc.tar.xz")
set(AOBUS_LLVM_SDK_SHA256
  "d96c2cc1736f4eb7fa43cb9bbdf56d93551a9ae0a9aadb9c99c3c3b2b712a234")

set(_AOBUS_LLVM_SDK_REQUIRED_FILES
  "bin/clang-apply-replacements.exe"
  "bin/clang-format.exe"
  "bin/clang-tidy.exe"
  "include/clang-tidy/tool/ClangTidyMain.h"
  "lib/clang/22/include/stddef.h"
  "lib/clangTidyMain.lib"
  "lib/cmake/clang/ClangConfig.cmake"
  "lib/cmake/llvm/LLVMConfig.cmake"
)
set(_AOBUS_LLVM_SDK_COMPLETION_MARKER ".aobus-llvm-sdk-complete")
set(_AOBUS_LLVM_SDK_COMPLETION_CONTENT
  "version=${AOBUS_LLVM_SDK_VERSION}\nsha256=${AOBUS_LLVM_SDK_SHA256}\n")

set(AOBUS_LLVM_SDK_ROOT "" CACHE PATH
  "Pre-provisioned clang+llvm ${AOBUS_LLVM_SDK_VERSION} root; empty downloads the official archive")

if(DEFINED ENV{AOBUS_LLVM_SDK_CACHE_ROOT} AND
   NOT "$ENV{AOBUS_LLVM_SDK_CACHE_ROOT}" STREQUAL "")
  set(_AOBUS_LLVM_SDK_CACHE_DEFAULT "$ENV{AOBUS_LLVM_SDK_CACHE_ROOT}")
elseif(DEFINED ENV{AOBUS_STATE_ROOT} AND NOT "$ENV{AOBUS_STATE_ROOT}" STREQUAL "")
  set(_AOBUS_LLVM_SDK_CACHE_DEFAULT "$ENV{AOBUS_STATE_ROOT}/cache/llvm")
elseif(DEFINED ENV{LOCALAPPDATA} AND NOT "$ENV{LOCALAPPDATA}" STREQUAL "")
  set(_AOBUS_LLVM_SDK_CACHE_DEFAULT "$ENV{LOCALAPPDATA}/Aobus/cache/llvm")
else()
  # The function is Windows-only. Keep the module parseable on other hosts,
  # then fail closed if an unusual Windows shell has no local state location.
  set(_AOBUS_LLVM_SDK_CACHE_DEFAULT "")
endif()
set(AOBUS_LLVM_SDK_CACHE_ROOT "${_AOBUS_LLVM_SDK_CACHE_DEFAULT}" CACHE PATH
  "Local cache root for the verified automatic Windows LLVM SDK")
unset(_AOBUS_LLVM_SDK_CACHE_DEFAULT)

function(_aobus_validate_llvm_sdk root require_completion_marker out_valid out_reason)
  set(_aobus_llvm_validation_issues "")
  foreach(_aobus_llvm_required_file IN LISTS _AOBUS_LLVM_SDK_REQUIRED_FILES)
    if(NOT EXISTS "${root}/${_aobus_llvm_required_file}")
      list(APPEND _aobus_llvm_validation_issues
        "missing ${root}/${_aobus_llvm_required_file}")
    endif()
  endforeach()

  if(require_completion_marker)
    set(_aobus_llvm_marker "${root}/${_AOBUS_LLVM_SDK_COMPLETION_MARKER}")
    if(NOT EXISTS "${_aobus_llvm_marker}")
      list(APPEND _aobus_llvm_validation_issues
        "missing completion marker ${_aobus_llvm_marker}")
    else()
      file(READ "${_aobus_llvm_marker}" _aobus_llvm_marker_content)
      if(NOT _aobus_llvm_marker_content STREQUAL _AOBUS_LLVM_SDK_COMPLETION_CONTENT)
        list(APPEND _aobus_llvm_validation_issues
          "completion marker does not match LLVM ${AOBUS_LLVM_SDK_VERSION} and its pinned SHA-256")
      endif()
    endif()
  endif()

  if(_aobus_llvm_validation_issues)
    string(JOIN "; " _aobus_llvm_validation_reason ${_aobus_llvm_validation_issues})
    set(${out_valid} FALSE PARENT_SCOPE)
    set(${out_reason} "${_aobus_llvm_validation_reason}" PARENT_SCOPE)
  else()
    set(${out_valid} TRUE PARENT_SCOPE)
    set(${out_reason} "" PARENT_SCOPE)
  endif()
endfunction()

function(aobus_configure_llvm_sdk)
  if(NOT WIN32)
    message(FATAL_ERROR "The prebuilt Aobus LLVM SDK bootstrap is Windows-only.")
  endif()

  if(AOBUS_LLVM_SDK_ROOT)
    cmake_path(ABSOLUTE_PATH AOBUS_LLVM_SDK_ROOT NORMALIZE
      BASE_DIRECTORY "${CMAKE_SOURCE_DIR}"
      OUTPUT_VARIABLE _aobus_llvm_sdk_root)
    _aobus_validate_llvm_sdk(
      "${_aobus_llvm_sdk_root}" FALSE _aobus_llvm_sdk_valid _aobus_llvm_sdk_reason)
    if(NOT _aobus_llvm_sdk_valid)
      message(FATAL_ERROR
        "Pre-provisioned LLVM SDK ${AOBUS_LLVM_SDK_VERSION} is incomplete: "
        "${_aobus_llvm_sdk_reason}. Re-extract ${AOBUS_LLVM_SDK_ARCHIVE}, or configure "
        "with -DAOBUS_LLVM_SDK_ROOT= to restore the verified automatic download.")
    endif()
    message(STATUS "Using pre-provisioned LLVM SDK: ${_aobus_llvm_sdk_root}")
  else()
    cmake_path(ABSOLUTE_PATH AOBUS_LLVM_SDK_CACHE_ROOT NORMALIZE
      BASE_DIRECTORY "${CMAKE_SOURCE_DIR}"
      OUTPUT_VARIABLE _aobus_llvm_sdk_cache_root)
    if(NOT _aobus_llvm_sdk_cache_root)
      message(FATAL_ERROR
        "AOBUS_LLVM_SDK_CACHE_ROOT is empty; set it to a local Windows directory.")
    endif()
    set(_aobus_llvm_sdk_cache
      "${_aobus_llvm_sdk_cache_root}/toolchains/llvm-${AOBUS_LLVM_SDK_VERSION}-x86_64-windows-msvc")
    set(_aobus_llvm_sdk_root "${_aobus_llvm_sdk_cache}")
    file(MAKE_DIRECTORY
      "${_aobus_llvm_sdk_cache_root}/toolchains"
      "${_aobus_llvm_sdk_cache_root}/downloads")
    file(LOCK "${_aobus_llvm_sdk_cache_root}/llvm-${AOBUS_LLVM_SDK_VERSION}.lock"
      GUARD FUNCTION TIMEOUT 1800)

    _aobus_validate_llvm_sdk(
      "${_aobus_llvm_sdk_cache}" TRUE _aobus_llvm_sdk_valid _aobus_llvm_sdk_reason)
    if(_aobus_llvm_sdk_valid)
      message(STATUS "Reusing verified LLVM SDK: ${_aobus_llvm_sdk_cache}")
    else()
      if(EXISTS "${_aobus_llvm_sdk_cache}")
        message(STATUS
          "Removing invalid LLVM SDK cache (${_aobus_llvm_sdk_reason}): "
          "${_aobus_llvm_sdk_cache}")
        file(REMOVE_RECURSE "${_aobus_llvm_sdk_cache}")
      endif()

      include(FetchContent)
      # FetchContent persists population stamps in the build tree. Remove only
      # this content's state so an invalid shared cache is actually re-extracted.
      if(NOT FETCHCONTENT_BASE_DIR)
        message(FATAL_ERROR
          "FETCHCONTENT_BASE_DIR is empty; refusing to remove LLVM SDK population state.")
      endif()
      cmake_path(ABSOLUTE_PATH FETCHCONTENT_BASE_DIR NORMALIZE
        BASE_DIRECTORY "${CMAKE_BINARY_DIR}"
        OUTPUT_VARIABLE _aobus_fetchcontent_base_dir)
      cmake_path(GET _aobus_fetchcontent_base_dir ROOT_PATH _aobus_fetchcontent_root)
      if(_aobus_fetchcontent_base_dir STREQUAL _aobus_fetchcontent_root)
        message(FATAL_ERROR
          "FETCHCONTENT_BASE_DIR resolves to a filesystem root; refusing to remove LLVM SDK population state: "
          "${_aobus_fetchcontent_base_dir}")
      endif()
      file(REMOVE_RECURSE
        "${_aobus_fetchcontent_base_dir}/aobus_llvm_sdk-build"
        "${_aobus_fetchcontent_base_dir}/aobus_llvm_sdk-subbuild")
      FetchContent_Declare(aobus_llvm_sdk
        URL "${AOBUS_LLVM_SDK_URL}"
        URL_HASH "SHA256=${AOBUS_LLVM_SDK_SHA256}"
        DOWNLOAD_DIR "${_aobus_llvm_sdk_cache_root}/downloads/llvm-${AOBUS_LLVM_SDK_VERSION}"
        SOURCE_DIR "${_aobus_llvm_sdk_cache}"
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
      )
      message(STATUS
        "Provisioning LLVM ${AOBUS_LLVM_SDK_VERSION} Windows SDK "
        "(${AOBUS_LLVM_SDK_ARCHIVE})")
      FetchContent_MakeAvailable(aobus_llvm_sdk)

      _aobus_validate_llvm_sdk(
        "${_aobus_llvm_sdk_cache}" FALSE _aobus_llvm_sdk_valid _aobus_llvm_sdk_reason)
      if(NOT _aobus_llvm_sdk_valid)
        file(REMOVE_RECURSE "${_aobus_llvm_sdk_cache}")
        message(FATAL_ERROR
          "Verified LLVM SDK extraction was incomplete (${_aobus_llvm_sdk_reason}). "
          "The invalid shared cache was removed; rerun CMake to retry, or set "
          "AOBUS_LLVM_SDK_ROOT to a complete extracted ${AOBUS_LLVM_SDK_ARCHIVE} directory.")
      endif()

      file(WRITE
        "${_aobus_llvm_sdk_cache}/${_AOBUS_LLVM_SDK_COMPLETION_MARKER}"
        "${_AOBUS_LLVM_SDK_COMPLETION_CONTENT}")
      message(STATUS "Verified LLVM SDK cache: ${_aobus_llvm_sdk_cache}")
    endif()
  endif()

  set(AOBUS_LLVM_SDK_RESOLVED_ROOT "${_aobus_llvm_sdk_root}" CACHE INTERNAL
    "Resolved clang+llvm SDK root used by the Windows lint toolchain" FORCE)
  set(AOBUS_LLVM_SDK_RESOLVED_VERSION "${AOBUS_LLVM_SDK_VERSION}" CACHE INTERNAL
    "Resolved clang+llvm SDK version used by the Windows lint toolchain" FORCE)
  set(LLVM_DIR "${_aobus_llvm_sdk_root}/lib/cmake/llvm" CACHE PATH
    "LLVM package used by the Aobus lint toolchain" FORCE)
  set(Clang_DIR "${_aobus_llvm_sdk_root}/lib/cmake/clang" CACHE PATH
    "Clang package used by the Aobus lint toolchain" FORCE)

  set(AOBUS_LLVM_SDK_RESOLVED_ROOT "${_aobus_llvm_sdk_root}" PARENT_SCOPE)
  set(LLVM_DIR "${LLVM_DIR}" PARENT_SCOPE)
  set(Clang_DIR "${Clang_DIR}" PARENT_SCOPE)
endfunction()
