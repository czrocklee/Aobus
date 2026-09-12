# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2026 Aobus Contributors
#
# Keeps one frontend's vocabulary out of the shared UI model.
#
# The include guardrails cannot see this class of defect. A frontend-owned
# LayoutSchema.h can hold a WinUI component catalog, a WinUI element lattice,
# and WinUI style rules, yet include nothing platform-specific at all, because
# deciding what a shell accepts is portable C++. What gives it away is its
# ownership: a file inside ao_app_uimodel that serves one frontend is by that
# admission not shared. Such a file belongs to that frontend - see
# app/windows-winui for where the WinUI half went.

include("${CMAKE_CURRENT_LIST_DIR}/AoSourceCode.cmake")

if(NOT PUBLIC_ROOT)
  message(FATAL_ERROR "AssertUimodelFrontendNeutrality: PUBLIC_ROOT not specified")
endif()

if(NOT SOURCE_ROOT)
  message(FATAL_ERROR "AssertUimodelFrontendNeutrality: SOURCE_ROOT not specified")
endif()

if(NOT TEST_ROOT)
  message(FATAL_ERROR "AssertUimodelFrontendNeutrality: TEST_ROOT not specified")
endif()

# Each frontend Aobus ships, spelled as it would appear at the head of a name.
set(_ao_frontend_name_regex "^(Windows|WinUi|WinUI|Gtk|GTK|Linux|Tui|Cli|Cocoa|AppKit|Qt)([._A-Z0-9]|$)")

# A frontend's API vocabulary, which no shared file has a reason to spell in
# code. Comments are exempt on purpose: explaining that GTK derives expansion
# from a widget's children is why a shared field is optional, and that reason
# belongs next to the field. Reaching for the type in code is a different act.
set(_ao_frontend_vocabulary_regex "(winrt::|Xaml|gtkmm|Gtk::|Gdk::|Glib::|Gio::|Pango::|Cairo::|ftxui::|ftxui/|#[ \t]*(include|import)[ \t]*[<\"](AppKit|Cocoa|Foundation|CoreGraphics)/|@[ \t]*import[ \t]+(AppKit|Cocoa|Foundation|CoreGraphics)[ \t]*([.]|;)|(^|[^A-Za-z0-9_])((NS|CG)[A-Z_]|kCG[A-Z])[A-Za-z0-9_]*)")

function(_ao_assert_frontend_neutral root label)
  if(NOT IS_DIRECTORY "${root}")
    message(FATAL_ERROR
            "AssertUimodelFrontendNeutrality: ${label} root is not a directory: ${root}")
  endif()

  file(GLOB_RECURSE _files LIST_DIRECTORIES false
       "${root}/*.h"
       "${root}/*.hpp"
       "${root}/*.cpp"
       "${root}/*.mm")

  foreach(_file IN LISTS _files)
    file(RELATIVE_PATH _rel "${root}" "${_file}")
    get_filename_component(_name "${_file}" NAME)

    if(_name MATCHES "${_ao_frontend_name_regex}")
      message(FATAL_ERROR
              "AssertUimodelFrontendNeutrality: ${label} file names a frontend, so it is that frontend's own: ${_rel}")
    endif()

    ao_read_code_text(_code "${_file}")
    if(_code MATCHES "${_ao_frontend_vocabulary_regex}")
      string(REGEX MATCH "${_ao_frontend_vocabulary_regex}" _match "${_code}")
      message(FATAL_ERROR
              "AssertUimodelFrontendNeutrality: ${label} file speaks a frontend's vocabulary: ${_rel}: ${_match}")
    endif()
  endforeach()
endfunction()

_ao_assert_frontend_neutral("${PUBLIC_ROOT}" "public")
_ao_assert_frontend_neutral("${SOURCE_ROOT}" "source")
_ao_assert_frontend_neutral("${TEST_ROOT}" "test")
