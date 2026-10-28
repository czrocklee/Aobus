# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2026 Aobus Contributors
#
# Guards the UIModel namespace and final folder layout rules.

foreach(_root_name IN ITEMS PUBLIC_ROOT SOURCE_ROOT TEST_ROOT)
  if(NOT DEFINED ${_root_name} OR "${${_root_name}}" STREQUAL "")
    message(FATAL_ERROR "AssertUimodelOrganization: ${_root_name} not specified")
  endif()
  if(NOT IS_ABSOLUTE "${${_root_name}}")
    set(${_root_name} "${CMAKE_CURRENT_SOURCE_DIR}/${${_root_name}}")
  endif()
  if(NOT IS_DIRECTORY "${${_root_name}}")
    message(FATAL_ERROR
            "AssertUimodelOrganization: ${_root_name} is not a directory: ${${_root_name}}")
  endif()
endforeach()

set(_ao_uimodel_allowed_path_regex
    "^(FrameClock\\.h|field/[^/]+\\.(h|hpp|cpp)|input/[^/]+\\.(h|hpp|cpp)|preference/[^/]+\\.(h|hpp|cpp)|presentation/[^/]+\\.(h|hpp|cpp)|layout/(document|component|shell)/[^/]+\\.(h|hpp|cpp)|library/(list|presentation|track|detail|property|task)/[^/]+\\.(h|hpp|cpp)|playback/(command|now-playing|transport|seek|output|quality|soul)/[^/]+\\.(h|hpp|cpp)|status/activity/[^/]+\\.(h|hpp|cpp))$")

function(_ao_collect_uimodel_files out_var root)
  file(GLOB_RECURSE _files LIST_DIRECTORIES false
       "${root}/*.h"
       "${root}/*.hpp"
       "${root}/*.cpp")
  set(${out_var} ${_files} PARENT_SCOPE)
endfunction()

function(_ao_assert_final_layout root label)
  _ao_collect_uimodel_files(_files "${root}")

  foreach(_file IN LISTS _files)
    file(RELATIVE_PATH _rel "${root}" "${_file}")
    if(NOT _rel MATCHES "${_ao_uimodel_allowed_path_regex}")
      message(FATAL_ERROR
              "AssertUimodelOrganization: ${label} file is outside the final UIModel folder layout: ${_rel}")
    endif()
  endforeach()
endfunction()

function(_ao_assert_public_namespaces root)
  file(GLOB_RECURSE _headers LIST_DIRECTORIES false
       "${root}/*.h"
       "${root}/*.hpp")

  foreach(_header IN LISTS _headers)
    file(STRINGS "${_header}" _lines)

    foreach(_line IN LISTS _lines)
      string(REGEX REPLACE "//.*$" "" _code "${_line}")

      if(_code MATCHES "^[ \t]*namespace[ \t]+ao::uimodel::([^ \t{;]+)")
        set(_suffix "${CMAKE_MATCH_1}")
        if(NOT _suffix STREQUAL "detail")
          message(FATAL_ERROR
                  "AssertUimodelOrganization: public UIModel header declares a feature namespace in ${_header}: ${_line}")
        endif()
      endif()
    endforeach()
  endforeach()
endfunction()

function(_ao_assert_test_namespaces root)
  _ao_collect_uimodel_files(_files "${root}")

  foreach(_file IN LISTS _files)
    file(STRINGS "${_file}" _lines)

    foreach(_line IN LISTS _lines)
      string(REGEX REPLACE "//.*$" "" _code "${_line}")

      if(_code MATCHES "^[ \t]*namespace[ \t]+ao::uimodel::([^ \t{;]+)")
        set(_suffix "${CMAKE_MATCH_1}")
        if(NOT _suffix STREQUAL "test")
          message(FATAL_ERROR
                  "AssertUimodelOrganization: UIModel test declares a feature-specific test namespace in ${_file}: ${_line}")
        endif()
      endif()
    endforeach()
  endforeach()
endfunction()

_ao_assert_final_layout("${PUBLIC_ROOT}" "public")
_ao_assert_final_layout("${SOURCE_ROOT}" "source")
_ao_assert_final_layout("${TEST_ROOT}" "test")
_ao_assert_public_namespaces("${PUBLIC_ROOT}")
_ao_assert_test_namespaces("${TEST_ROOT}")
