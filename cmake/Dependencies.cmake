# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2026 Aobus Contributors
#
# External dependency discovery and INTERFACE wrapper targets.

set(AOBUS_DEPENDENCY_CONTRACT_FILE "${CMAKE_SOURCE_DIR}/dependency-contract.json")
if(NOT EXISTS "${AOBUS_DEPENDENCY_CONTRACT_FILE}")
  message(FATAL_ERROR "Aobus dependency contract is missing: ${AOBUS_DEPENDENCY_CONTRACT_FILE}")
endif()
file(READ "${AOBUS_DEPENDENCY_CONTRACT_FILE}" AOBUS_DEPENDENCY_CONTRACT)
file(SHA256 "${AOBUS_DEPENDENCY_CONTRACT_FILE}" AOBUS_DEPENDENCY_CONTRACT_SHA256)

# The dependency report embeds hashes of these files; reconfigure when they
# change so a plain `cmake --build` cannot keep serving a stale report.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${AOBUS_DEPENDENCY_CONTRACT_FILE}"
  "${CMAKE_SOURCE_DIR}/vcpkg-configuration.json"
  "${CMAKE_SOURCE_DIR}/vcpkg.json")

function(aobus_contract_get output)
  string(JSON value ERROR_VARIABLE error GET "${AOBUS_DEPENDENCY_CONTRACT}" ${ARGN})
  if(NOT error STREQUAL "NOTFOUND")
    string(JOIN "." field ${ARGN})
    message(FATAL_ERROR
      "Invalid Aobus dependency contract ${AOBUS_DEPENDENCY_CONTRACT_FILE}: "
      "cannot read '${field}': ${error}")
  endif()
  set(${output} "${value}" PARENT_SCOPE)
endfunction()

function(aobus_contract_optional_get output found)
  string(JSON value ERROR_VARIABLE error GET "${AOBUS_DEPENDENCY_CONTRACT}" ${ARGN})
  if(error STREQUAL "NOTFOUND")
    set(${output} "${value}" PARENT_SCOPE)
    set(${found} TRUE PARENT_SCOPE)
  else()
    set(${output} "" PARENT_SCOPE)
    set(${found} FALSE PARENT_SCOPE)
  endif()
endfunction()

aobus_contract_get(AOBUS_DEPENDENCY_SCHEMA_VERSION schemaVersion)
if(NOT AOBUS_DEPENDENCY_SCHEMA_VERSION EQUAL 1)
  message(FATAL_ERROR
    "Unsupported dependency contract schema ${AOBUS_DEPENDENCY_SCHEMA_VERSION} in "
    "${AOBUS_DEPENDENCY_CONTRACT_FILE}; Aobus supports schema 1.")
endif()

if(WIN32)
  set(AOBUS_DEPENDENCY_PLATFORM "windows")
else()
  set(AOBUS_DEPENDENCY_PLATFORM "linux")
endif()

function(aobus_dependency_policy dependency output_kind output_version output_exception)
  aobus_contract_get(kind dependencies "${dependency}" policy kind)
  if(kind STREQUAL "exact")
    aobus_contract_get(version dependencies "${dependency}" policy version)
  elseif(kind STREQUAL "range")
    aobus_contract_get(minimum dependencies "${dependency}" policy minimum)
    aobus_contract_optional_get(maximum has_maximum
      dependencies "${dependency}" policy exclusiveMaximum)
    if(has_maximum)
      set(version "${minimum}...<${maximum}")
    else()
      set(version "${minimum}")
    endif()
  else()
    message(FATAL_ERROR
      "Invalid Aobus dependency contract ${AOBUS_DEPENDENCY_CONTRACT_FILE}: "
      "dependency '${dependency}' has unsupported policy kind '${kind}'.")
  endif()

  set(exception_id "")
  string(JSON exception_count ERROR_VARIABLE exception_error
    LENGTH "${AOBUS_DEPENDENCY_CONTRACT}" exceptions)
  if(NOT exception_error STREQUAL "NOTFOUND")
    message(FATAL_ERROR
      "Invalid Aobus dependency contract ${AOBUS_DEPENDENCY_CONTRACT_FILE}: "
      "cannot read exceptions: ${exception_error}")
  endif()
  if(exception_count GREATER 0)
    math(EXPR exception_last "${exception_count} - 1")
    string(TIMESTAMP today "%Y-%m-%d" UTC)
    foreach(index RANGE 0 ${exception_last})
      aobus_contract_get(exception_dependency exceptions ${index} dependency)
      aobus_contract_get(exception_platform exceptions ${index} platform)
      if("${exception_dependency}" STREQUAL "${dependency}"
         AND "${exception_platform}" STREQUAL "${AOBUS_DEPENDENCY_PLATFORM}")
        if(exception_id)
          message(FATAL_ERROR
            "Invalid Aobus dependency contract ${AOBUS_DEPENDENCY_CONTRACT_FILE}: multiple active "
            "exceptions target '${dependency}' on '${AOBUS_DEPENDENCY_PLATFORM}'.")
        endif()
        aobus_contract_get(exception_id exceptions ${index} id)
        aobus_contract_get(expires exceptions ${index} expires)
        if("${expires}" STRLESS "${today}")
          message(FATAL_ERROR
            "Dependency exception '${exception_id}' for '${dependency}' on "
            "'${AOBUS_DEPENDENCY_PLATFORM}' expired on ${expires}. Remove or renew it before configuring.")
        endif()
        aobus_contract_get(version exceptions ${index} allowedVersion)
        set(kind "exact")
      endif()
    endforeach()
  endif()

  set(${output_kind} "${kind}" PARENT_SCOPE)
  set(${output_version} "${version}" PARENT_SCOPE)
  set(${output_exception} "${exception_id}" PARENT_SCOPE)
endfunction()

function(aobus_policy_find_arguments dependency output_arguments output_version output_exception)
  aobus_dependency_policy("${dependency}" kind version exception_id)
  if(kind STREQUAL "exact")
    set(arguments "${version};EXACT")
  else()
    set(arguments "${version}")
  endif()
  set(${output_arguments} "${arguments}" PARENT_SCOPE)
  set(${output_version} "${version}" PARENT_SCOPE)
  set(${output_exception} "${exception_id}" PARENT_SCOPE)
endfunction()

aobus_policy_find_arguments(boost AOBUS_BOOST_FIND_ARGUMENTS AOBUS_BOOST_REQUESTED_VERSION AOBUS_BOOST_EXCEPTION)
aobus_policy_find_arguments(spdlog AOBUS_SPDLOG_FIND_ARGUMENTS AOBUS_SPDLOG_REQUESTED_VERSION AOBUS_SPDLOG_EXCEPTION)
aobus_policy_find_arguments(ftxui AOBUS_FTXUI_FIND_ARGUMENTS AOBUS_FTXUI_REQUESTED_VERSION AOBUS_FTXUI_EXCEPTION)

aobus_contract_optional_get(AOBUS_FTXUI_REQUIRED_WHEN AOBUS_FTXUI_HAS_CONDITION
  dependencies ftxui cmake requiredWhen)
if(NOT AOBUS_FTXUI_HAS_CONDITION OR NOT AOBUS_FTXUI_REQUIRED_WHEN STREQUAL "AOBUS_BUILD_TUI")
  message(FATAL_ERROR
    "Invalid Aobus dependency contract ${AOBUS_DEPENDENCY_CONTRACT_FILE}: ftxui.requiredWhen "
    "must be the supported build option AOBUS_BUILD_TUI.")
endif()

find_package(Boost ${AOBUS_BOOST_FIND_ARGUMENTS} QUIET COMPONENTS headers process filesystem)
if(NOT Boost_FOUND)
  message(FATAL_ERROR
    "Aobus dependency 'boost' requires ${AOBUS_BOOST_REQUESTED_VERSION}, but the active resolver "
    "could not provide an exact compatible package. Considered versions: "
    "'${Boost_CONSIDERED_VERSIONS}'. See doc/development/dependency-upgrade.md.")
endif()
find_package(spdlog ${AOBUS_SPDLOG_FIND_ARGUMENTS} CONFIG QUIET)
if(NOT spdlog_FOUND)
  message(FATAL_ERROR
    "Aobus dependency 'spdlog' requires ${AOBUS_SPDLOG_REQUESTED_VERSION}, but the active resolver "
    "could not provide an exact compatible package. Considered versions: "
    "'${spdlog_CONSIDERED_VERSIONS}'. See doc/development/dependency-upgrade.md.")
endif()
find_package(ryml CONFIG REQUIRED)
find_package(c4core CONFIG REQUIRED)
find_package(mimalloc CONFIG REQUIRED)
find_package(gsl-lite CONFIG REQUIRED)
find_package(lexy CONFIG QUIET)
if(NOT TARGET foonathan::lexy)
  find_path(AOBUS_LEXY_INCLUDE_DIR NAMES lexy/grammar.hpp REQUIRED)
  add_library(AobusLexy INTERFACE)
  add_library(foonathan::lexy ALIAS AobusLexy)
  target_include_directories(AobusLexy SYSTEM INTERFACE "${AOBUS_LEXY_INCLUDE_DIR}")
endif()

if(AOBUS_BUILD_TESTS)
  find_package(Catch2 3 REQUIRED)
  find_package(FakeIt CONFIG REQUIRED)
endif()

if(AOBUS_BUILD_CLI OR AOBUS_BUILD_TUI OR AOBUS_BUILD_TOOLS)
  find_package(CLI11 CONFIG REQUIRED)
endif()

if(AOBUS_BUILD_TUI)
  find_package(ftxui ${AOBUS_FTXUI_FIND_ARGUMENTS} CONFIG QUIET)
  if(NOT ftxui_FOUND)
    message(FATAL_ERROR
      "Aobus dependency 'ftxui' requires ${AOBUS_FTXUI_REQUESTED_VERSION}, but the active resolver "
      "could not provide an exact compatible package. Considered versions: "
      "'${ftxui_CONSIDERED_VERSIONS}'. See doc/development/dependency-upgrade.md.")
  endif()

  # stb is header-only; vcpkg installs stb_image.h at the include root,
  # Linux distributions typically under include/stb/.
  find_path(AOBUS_STB_INCLUDE_DIR NAMES stb_image.h PATH_SUFFIXES stb REQUIRED)
  foreach(_stb_header IN ITEMS stb_image.h stb_image_resize2.h stb_image_write.h)
    if(NOT EXISTS "${AOBUS_STB_INCLUDE_DIR}/${_stb_header}")
      message(FATAL_ERROR "Aobus TUI requires ${_stb_header} in ${AOBUS_STB_INCLUDE_DIR}.")
    endif()
  endforeach()
  add_library(PkgStb INTERFACE)
  target_include_directories(PkgStb SYSTEM INTERFACE "${AOBUS_STB_INCLUDE_DIR}")
endif()

function(aobus_validate_dependency_targets dependency)
  string(JSON target_count ERROR_VARIABLE target_error
    LENGTH "${AOBUS_DEPENDENCY_CONTRACT}" dependencies "${dependency}" cmake targets)
  if(NOT target_error STREQUAL "NOTFOUND" OR target_count EQUAL 0)
    message(FATAL_ERROR
      "Invalid Aobus dependency contract ${AOBUS_DEPENDENCY_CONTRACT_FILE}: "
      "dependency '${dependency}' must declare at least one CMake target.")
  endif()
  math(EXPR target_last "${target_count} - 1")
  foreach(index RANGE 0 ${target_last})
    aobus_contract_get(required_target dependencies "${dependency}" cmake targets ${index})
    if(NOT TARGET "${required_target}")
      message(FATAL_ERROR
        "Aobus dependency '${dependency}' resolved without required CMake target "
        "'${required_target}'. See doc/development/dependency-upgrade.md.")
    endif()
  endforeach()
endfunction()

aobus_validate_dependency_targets(boost)
aobus_validate_dependency_targets(spdlog)
if(AOBUS_BUILD_TUI)
  aobus_validate_dependency_targets(ftxui)
endif()

if(DEFINED Boost_VERSION_STRING)
  set(AOBUS_BOOST_RESOLVED_VERSION "${Boost_VERSION_STRING}")
else()
  set(AOBUS_BOOST_RESOLVED_VERSION "${Boost_VERSION}")
endif()
set(AOBUS_SPDLOG_RESOLVED_VERSION "${spdlog_VERSION}")
if(AOBUS_BUILD_TUI)
  set(AOBUS_FTXUI_RESOLVED_VERSION "${ftxui_VERSION}")
endif()

add_library(PkgRapidYaml INTERFACE)
target_link_libraries(PkgRapidYaml INTERFACE ryml::ryml c4core::c4core)
target_compile_definitions(PkgRapidYaml INTERFACE $<$<CXX_COMPILER_ID:MSVC>:C4_CPP=17>)

get_target_property(AOBUS_RYML_INCLUDE_DIRS ryml::ryml INTERFACE_INCLUDE_DIRECTORIES)
if(AOBUS_RYML_INCLUDE_DIRS)
  foreach(_ryml_include_dir IN LISTS AOBUS_RYML_INCLUDE_DIRS)
    if(EXISTS "${_ryml_include_dir}/ryml/ryml.hpp")
      target_include_directories(PkgRapidYaml INTERFACE "${_ryml_include_dir}/ryml")
    endif()
  endforeach()
endif()

# Aobus compiles spdlog call sites with SPDLOG_USE_STD_FORMAT. If the located
# spdlog package was built against fmt instead (e.g. vcpkg's default 'fmt'
# feature, or a stale CMake cache), the build can succeed but fail at runtime
# with missing log_msg(std::string_view) symbols. Fail during configure instead.
get_target_property(AOBUS_SPDLOG_COMPILE_DEFINITIONS spdlog::spdlog INTERFACE_COMPILE_DEFINITIONS)
if(NOT AOBUS_SPDLOG_COMPILE_DEFINITIONS)
  set(AOBUS_SPDLOG_COMPILE_DEFINITIONS "")
endif()

if(NOT SPDLOG_USE_STD_FORMAT IN_LIST AOBUS_SPDLOG_COMPILE_DEFINITIONS)
  message(FATAL_ERROR
    "Aobus requires spdlog built with SPDLOG_USE_STD_FORMAT, but the located "
    "spdlog package does not advertise that ABI. spdlog_DIR='${spdlog_DIR}', "
    "INTERFACE_COMPILE_DEFINITIONS='${AOBUS_SPDLOG_COMPILE_DEFINITIONS}'.")
endif()

if(SPDLOG_FMT_EXTERNAL IN_LIST AOBUS_SPDLOG_COMPILE_DEFINITIONS)
  message(FATAL_ERROR
    "Aobus requires std::format-backed spdlog, but the located spdlog target "
    "uses SPDLOG_FMT_EXTERNAL. spdlog_DIR='${spdlog_DIR}'.")
endif()

if(NOT AOBUS_BOOST_RESOLVED_VERSION OR NOT AOBUS_SPDLOG_RESOLVED_VERSION
   OR (AOBUS_BUILD_TUI AND NOT AOBUS_FTXUI_RESOLVED_VERSION))
  message(FATAL_ERROR
    "A governed dependency package did not expose its resolved version. "
    "Boost='${AOBUS_BOOST_RESOLVED_VERSION}', spdlog='${AOBUS_SPDLOG_RESOLVED_VERSION}', "
    "ftxui='${AOBUS_FTXUI_RESOLVED_VERSION}'. See doc/development/dependency-upgrade.md.")
endif()

function(aobus_json_quote output value)
  string(REPLACE "\\" "\\\\" escaped "${value}")
  string(REPLACE "\"" "\\\"" escaped "${escaped}")
  string(REPLACE "\n" "\\n" escaped "${escaped}")
  string(REPLACE "\r" "\\r" escaped "${escaped}")
  string(REPLACE "\t" "\\t" escaped "${escaped}")
  set(${output} "\"${escaped}\"" PARENT_SCOPE)
endfunction()

function(aobus_json_set_string output document field value)
  aobus_json_quote(quoted "${value}")
  string(JSON updated SET "${document}" "${field}" "${quoted}")
  set(${output} "${updated}" PARENT_SCOPE)
endfunction()

function(aobus_dependency_target_results dependency output)
  string(JSON target_count LENGTH
    "${AOBUS_DEPENDENCY_CONTRACT}" dependencies "${dependency}" cmake targets)
  math(EXPR target_last "${target_count} - 1")
  set(results "{}")
  foreach(index RANGE 0 ${target_last})
    aobus_contract_get(required_target dependencies "${dependency}" cmake targets ${index})
    string(JSON results SET "${results}" "${required_target}" true)
  endforeach()
  set(${output} "${results}" PARENT_SCOPE)
endfunction()

function(aobus_dependency_report_entry output dependency requested resolved exception targets capabilities)
  aobus_dependency_policy("${dependency}" policy_kind ignored_version ignored_exception)
  set(entry "{}")
  aobus_json_set_string(entry "${entry}" status verified)
  aobus_json_set_string(entry "${entry}" policyKind "${policy_kind}")
  aobus_json_set_string(entry "${entry}" requestedVersion "${requested}")
  aobus_json_set_string(entry "${entry}" resolvedVersion "${resolved}")
  if(exception)
    aobus_json_quote(quoted_exception "${exception}")
    string(JSON entry SET "${entry}" exceptionId "${quoted_exception}")
  else()
    string(JSON entry SET "${entry}" exceptionId null)
  endif()
  string(JSON entry SET "${entry}" targets "${targets}")
  string(JSON entry SET "${entry}" capabilities "${capabilities}")
  set(${output} "${entry}" PARENT_SCOPE)
endfunction()

aobus_dependency_target_results(boost AOBUS_BOOST_TARGET_RESULTS)
aobus_dependency_target_results(spdlog AOBUS_SPDLOG_TARGET_RESULTS)
if(AOBUS_BUILD_TUI)
  aobus_dependency_target_results(ftxui AOBUS_FTXUI_TARGET_RESULTS)
else()
  set(AOBUS_FTXUI_TARGET_RESULTS "{}")
endif()

set(AOBUS_SPDLOG_CAPABILITIES "{}")
string(JSON AOBUS_SPDLOG_CAPABILITIES SET
  "${AOBUS_SPDLOG_CAPABILITIES}" spdlog-std-format true)
string(JSON AOBUS_SPDLOG_CAPABILITIES SET
  "${AOBUS_SPDLOG_CAPABILITIES}" spdlog-no-external-fmt true)

aobus_dependency_report_entry(AOBUS_BOOST_REPORT boost
  "${AOBUS_BOOST_REQUESTED_VERSION}" "${AOBUS_BOOST_RESOLVED_VERSION}"
  "${AOBUS_BOOST_EXCEPTION}" "${AOBUS_BOOST_TARGET_RESULTS}" "{}")
aobus_dependency_report_entry(AOBUS_SPDLOG_REPORT spdlog
  "${AOBUS_SPDLOG_REQUESTED_VERSION}" "${AOBUS_SPDLOG_RESOLVED_VERSION}"
  "${AOBUS_SPDLOG_EXCEPTION}" "${AOBUS_SPDLOG_TARGET_RESULTS}" "${AOBUS_SPDLOG_CAPABILITIES}")
if(AOBUS_BUILD_TUI)
  aobus_dependency_report_entry(AOBUS_FTXUI_REPORT ftxui
    "${AOBUS_FTXUI_REQUESTED_VERSION}" "${AOBUS_FTXUI_RESOLVED_VERSION}"
    "${AOBUS_FTXUI_EXCEPTION}" "${AOBUS_FTXUI_TARGET_RESULTS}" "{}")
  set(AOBUS_FTXUI_CONDITION "{}")
  aobus_json_set_string(AOBUS_FTXUI_CONDITION
    "${AOBUS_FTXUI_CONDITION}" name "${AOBUS_FTXUI_REQUIRED_WHEN}")
  string(JSON AOBUS_FTXUI_CONDITION SET "${AOBUS_FTXUI_CONDITION}" value true)
  string(JSON AOBUS_FTXUI_REPORT SET
    "${AOBUS_FTXUI_REPORT}" condition "${AOBUS_FTXUI_CONDITION}")
else()
  aobus_dependency_policy(ftxui AOBUS_FTXUI_POLICY_KIND ignored_version ignored_exception)
  set(AOBUS_FTXUI_REPORT "{}")
  aobus_json_set_string(AOBUS_FTXUI_REPORT "${AOBUS_FTXUI_REPORT}" status not-applicable)
  aobus_json_set_string(AOBUS_FTXUI_REPORT
    "${AOBUS_FTXUI_REPORT}" policyKind "${AOBUS_FTXUI_POLICY_KIND}")
  aobus_json_set_string(AOBUS_FTXUI_REPORT
    "${AOBUS_FTXUI_REPORT}" requestedVersion "${AOBUS_FTXUI_REQUESTED_VERSION}")
  string(JSON AOBUS_FTXUI_REPORT SET "${AOBUS_FTXUI_REPORT}" resolvedVersion null)
  string(JSON AOBUS_FTXUI_REPORT SET "${AOBUS_FTXUI_REPORT}" exceptionId null)
  string(JSON AOBUS_FTXUI_REPORT SET "${AOBUS_FTXUI_REPORT}" targets "{}")
  string(JSON AOBUS_FTXUI_REPORT SET "${AOBUS_FTXUI_REPORT}" capabilities "{}")
  set(AOBUS_FTXUI_CONDITION "{}")
  aobus_json_set_string(AOBUS_FTXUI_CONDITION
    "${AOBUS_FTXUI_CONDITION}" name "${AOBUS_FTXUI_REQUIRED_WHEN}")
  string(JSON AOBUS_FTXUI_CONDITION SET "${AOBUS_FTXUI_CONDITION}" value false)
  string(JSON AOBUS_FTXUI_REPORT SET
    "${AOBUS_FTXUI_REPORT}" condition "${AOBUS_FTXUI_CONDITION}")
endif()

set(AOBUS_DEPENDENCY_REPORT "{}")
string(JSON AOBUS_DEPENDENCY_REPORT SET "${AOBUS_DEPENDENCY_REPORT}" schemaVersion 1)
string(JSON AOBUS_DEPENDENCY_REPORT SET "${AOBUS_DEPENDENCY_REPORT}" contract "{}")
string(JSON AOBUS_DEPENDENCY_REPORT SET
  "${AOBUS_DEPENDENCY_REPORT}" contract schemaVersion ${AOBUS_DEPENDENCY_SCHEMA_VERSION})
aobus_json_quote(quoted_contract_hash "${AOBUS_DEPENDENCY_CONTRACT_SHA256}")
string(JSON AOBUS_DEPENDENCY_REPORT SET
  "${AOBUS_DEPENDENCY_REPORT}" contract sha256 "${quoted_contract_hash}")

set(AOBUS_DEPENDENCY_HOST "{}")
aobus_json_set_string(AOBUS_DEPENDENCY_HOST
  "${AOBUS_DEPENDENCY_HOST}" platform "${AOBUS_DEPENDENCY_PLATFORM}")
aobus_json_set_string(AOBUS_DEPENDENCY_HOST
  "${AOBUS_DEPENDENCY_HOST}" cmakeVersion "${CMAKE_VERSION}")
set(AOBUS_DEPENDENCY_COMPILER "{}")
aobus_json_set_string(AOBUS_DEPENDENCY_COMPILER
  "${AOBUS_DEPENDENCY_COMPILER}" id "${CMAKE_CXX_COMPILER_ID}")
aobus_json_set_string(AOBUS_DEPENDENCY_COMPILER
  "${AOBUS_DEPENDENCY_COMPILER}" version "${CMAKE_CXX_COMPILER_VERSION}")
string(JSON AOBUS_DEPENDENCY_HOST SET
  "${AOBUS_DEPENDENCY_HOST}" compiler "${AOBUS_DEPENDENCY_COMPILER}")

if(DEFINED ENV{AOBUS_NIX_DEPENDENCY_REPORT} AND NOT "$ENV{AOBUS_NIX_DEPENDENCY_REPORT}" STREQUAL "")
  aobus_json_quote(quoted_nix_report "$ENV{AOBUS_NIX_DEPENDENCY_REPORT}")
  string(JSON AOBUS_DEPENDENCY_HOST SET
    "${AOBUS_DEPENDENCY_HOST}" nixReportPath "${quoted_nix_report}")
else()
  string(JSON AOBUS_DEPENDENCY_HOST SET "${AOBUS_DEPENDENCY_HOST}" nixReportPath null)
endif()

if(DEFINED VCPKG_TARGET_TRIPLET)
  aobus_json_quote(quoted_triplet "${VCPKG_TARGET_TRIPLET}")
  string(JSON AOBUS_DEPENDENCY_HOST SET "${AOBUS_DEPENDENCY_HOST}" vcpkgTriplet "${quoted_triplet}")
else()
  string(JSON AOBUS_DEPENDENCY_HOST SET "${AOBUS_DEPENDENCY_HOST}" vcpkgTriplet null)
endif()
if(DEFINED VCPKG_INSTALLED_DIR)
  aobus_json_quote(quoted_vcpkg_installed "${VCPKG_INSTALLED_DIR}")
  string(JSON AOBUS_DEPENDENCY_HOST SET
    "${AOBUS_DEPENDENCY_HOST}" vcpkgInstalledDir "${quoted_vcpkg_installed}")
else()
  string(JSON AOBUS_DEPENDENCY_HOST SET "${AOBUS_DEPENDENCY_HOST}" vcpkgInstalledDir null)
endif()

foreach(lock_file IN ITEMS vcpkg-configuration.json vcpkg.json)
  file(SHA256 "${CMAKE_SOURCE_DIR}/${lock_file}" lock_hash)
  aobus_json_quote(quoted_lock_hash "${lock_hash}")
  if(lock_file STREQUAL "vcpkg-configuration.json")
    set(lock_field vcpkgConfigurationSha256)
  else()
    set(lock_field vcpkgManifestSha256)
  endif()
  string(JSON AOBUS_DEPENDENCY_HOST SET
    "${AOBUS_DEPENDENCY_HOST}" "${lock_field}" "${quoted_lock_hash}")
endforeach()

if(WIN32)
  find_program(AOBUS_VCPKG_EXECUTABLE NAMES vcpkg.exe vcpkg HINTS "$ENV{VCPKG_ROOT}")
  if(AOBUS_VCPKG_EXECUTABLE)
    execute_process(
      COMMAND "${AOBUS_VCPKG_EXECUTABLE}" version
      RESULT_VARIABLE vcpkg_status
      OUTPUT_VARIABLE vcpkg_version
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE)
  endif()
  if(AOBUS_VCPKG_EXECUTABLE AND vcpkg_status EQUAL 0)
    aobus_json_quote(quoted_vcpkg_version "${vcpkg_version}")
    string(JSON AOBUS_DEPENDENCY_HOST SET
      "${AOBUS_DEPENDENCY_HOST}" vcpkgVersion "${quoted_vcpkg_version}")
  else()
    string(JSON AOBUS_DEPENDENCY_HOST SET "${AOBUS_DEPENDENCY_HOST}" vcpkgVersion null)
  endif()
else()
  string(JSON AOBUS_DEPENDENCY_HOST SET "${AOBUS_DEPENDENCY_HOST}" vcpkgVersion null)
endif()

string(JSON AOBUS_DEPENDENCY_REPORT SET
  "${AOBUS_DEPENDENCY_REPORT}" host "${AOBUS_DEPENDENCY_HOST}")
string(JSON AOBUS_DEPENDENCY_REPORT SET "${AOBUS_DEPENDENCY_REPORT}" dependencies "{}")
string(JSON AOBUS_DEPENDENCY_REPORT SET
  "${AOBUS_DEPENDENCY_REPORT}" dependencies boost "${AOBUS_BOOST_REPORT}")
string(JSON AOBUS_DEPENDENCY_REPORT SET
  "${AOBUS_DEPENDENCY_REPORT}" dependencies ftxui "${AOBUS_FTXUI_REPORT}")
string(JSON AOBUS_DEPENDENCY_REPORT SET
  "${AOBUS_DEPENDENCY_REPORT}" dependencies spdlog "${AOBUS_SPDLOG_REPORT}")
file(WRITE "${CMAKE_BINARY_DIR}/aobus-dependencies.json" "${AOBUS_DEPENDENCY_REPORT}\n")

find_program(GPERF_EXECUTABLE gperf REQUIRED)

if(WIN32)
  find_package(FLAC CONFIG REQUIRED)
  add_library(PkgFLAC INTERFACE)
  target_link_libraries(PkgFLAC INTERFACE FLAC::FLAC)

  set(AOBUS_ALAC_PREFIX "")
  if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(AOBUS_ALAC_PREFIX "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
  endif()

  find_path(ALAC_INCLUDE_DIR NAMES alac/ALACDecoder.h HINTS "${AOBUS_ALAC_PREFIX}/include" REQUIRED)
  find_library(ALAC_LIBRARY_RELEASE NAMES libalac alac HINTS "${AOBUS_ALAC_PREFIX}/lib" REQUIRED)
  find_library(ALAC_LIBRARY_DEBUG
    NAMES libalac alac
    HINTS "${AOBUS_ALAC_PREFIX}/debug/lib"
    NO_DEFAULT_PATH)
  if(NOT ALAC_LIBRARY_DEBUG)
    set(ALAC_LIBRARY_DEBUG "${ALAC_LIBRARY_RELEASE}")
  endif()
  add_library(PkgALAC INTERFACE)
  target_include_directories(PkgALAC INTERFACE "${ALAC_INCLUDE_DIR}")
  target_link_libraries(PkgALAC INTERFACE
    "$<$<CONFIG:Debug>:${ALAC_LIBRARY_DEBUG}>"
    "$<$<NOT:$<CONFIG:Debug>>:${ALAC_LIBRARY_RELEASE}>")

  find_package(fdk-aac CONFIG REQUIRED)
  add_library(PkgFDKAAC INTERFACE)
  target_link_libraries(PkgFDKAAC INTERFACE FDK-AAC::fdk-aac)

  find_package(mpg123 CONFIG REQUIRED)
  add_library(PkgMPG123 INTERFACE)
  target_link_libraries(PkgMPG123 INTERFACE MPG123::libmpg123)

  find_package(unofficial-lmdb CONFIG REQUIRED)
  add_library(PkgLMDB INTERFACE)
  target_link_libraries(PkgLMDB INTERFACE unofficial::lmdb::lmdb)

  find_package(xxHash CONFIG REQUIRED)
  add_library(PkgXXHash INTERFACE)
  target_link_libraries(PkgXXHash INTERFACE xxHash::xxhash)
else()
  find_package(PkgConfig REQUIRED)

  if(AOBUS_BUILD_GTK)
    find_program(GLIB_COMPILE_RESOURCES_EXECUTABLE glib-compile-resources REQUIRED)

    pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
    add_library(PkgGTK4 INTERFACE)
    target_link_libraries(PkgGTK4 INTERFACE PkgConfig::GTK4)

    pkg_check_modules(GTKMM REQUIRED IMPORTED_TARGET gtkmm-4.0)
    add_library(PkgGTKMM INTERFACE)
    target_link_libraries(PkgGTKMM INTERFACE PkgConfig::GTKMM)

    pkg_check_modules(GLIBMM REQUIRED IMPORTED_TARGET glibmm-2.68)
    add_library(PkgGLIBMM INTERFACE)
    target_link_libraries(PkgGLIBMM INTERFACE PkgConfig::GLIBMM)
  endif()

  pkg_check_modules(FLAC REQUIRED IMPORTED_TARGET flac)
  add_library(PkgFLAC INTERFACE)
  target_link_libraries(PkgFLAC INTERFACE PkgConfig::FLAC)

  pkg_check_modules(ALAC REQUIRED IMPORTED_TARGET alac)
  add_library(PkgALAC INTERFACE)
  target_link_libraries(PkgALAC INTERFACE PkgConfig::ALAC)

  pkg_check_modules(MPG123 REQUIRED IMPORTED_TARGET libmpg123)
  add_library(PkgMPG123 INTERFACE)
  target_link_libraries(PkgMPG123 INTERFACE PkgConfig::MPG123)

  pkg_check_modules(FDKAAC REQUIRED IMPORTED_TARGET fdk-aac)
  add_library(PkgFDKAAC INTERFACE)
  target_link_libraries(PkgFDKAAC INTERFACE PkgConfig::FDKAAC)

  # PipeWire and ALSA are the only Linux audio backends, so both are hard
  # requirements: lib/audio unconditionally compiles their providers.
  pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
  add_library(PkgPipeWire INTERFACE)
  target_link_libraries(PkgPipeWire INTERFACE PkgConfig::PIPEWIRE)

  pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
  add_library(PkgALSA INTERFACE)
  target_link_libraries(PkgALSA INTERFACE PkgConfig::ALSA)

  pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)
  add_library(PkgUDEV INTERFACE)
  target_link_libraries(PkgUDEV INTERFACE PkgConfig::UDEV)

  pkg_check_modules(LMDB REQUIRED IMPORTED_TARGET lmdb)
  add_library(PkgLMDB INTERFACE)
  target_link_libraries(PkgLMDB INTERFACE PkgConfig::LMDB)

  pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)
  add_library(PkgXXHash INTERFACE)
  target_link_libraries(PkgXXHash INTERFACE PkgConfig::XXHASH)
endif()
