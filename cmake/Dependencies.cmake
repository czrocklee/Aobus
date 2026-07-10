# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2026 Aobus Contributors
#
# External dependency discovery and INTERFACE wrapper targets.

find_package(Boost REQUIRED COMPONENTS headers process filesystem)
find_package(spdlog CONFIG REQUIRED)
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
  find_package(ftxui CONFIG REQUIRED)

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
  add_library(lmdb ALIAS unofficial::lmdb::lmdb)

  find_package(xxHash CONFIG REQUIRED)
  add_library(PkgXXHash INTERFACE)
  target_link_libraries(PkgXXHash INTERFACE xxHash::xxhash)
else()
  find_package(PkgConfig REQUIRED)

  if(AOBUS_BUILD_GTK)
    find_program(GLIB_COMPILE_RESOURCES_EXECUTABLE glib-compile-resources REQUIRED)

    pkg_check_modules(GTK4 REQUIRED gtk4)
    add_library(PkgGTK4 INTERFACE)
    target_include_directories(PkgGTK4 INTERFACE ${GTK4_INCLUDE_DIRS})
    target_link_libraries(PkgGTK4 INTERFACE ${GTK4_LIBRARIES})
    target_compile_options(PkgGTK4 INTERFACE ${GTK4_CFLAGS_OTHER})

    pkg_check_modules(GTKMM REQUIRED gtkmm-4.0)
    add_library(PkgGTKMM INTERFACE)
    target_include_directories(PkgGTKMM INTERFACE ${GTKMM_INCLUDE_DIRS})
    target_link_libraries(PkgGTKMM INTERFACE ${GTKMM_LIBRARIES})
    target_compile_options(PkgGTKMM INTERFACE ${GTKMM_CFLAGS_OTHER})

    pkg_check_modules(GLIBMM REQUIRED glibmm-2.68)
    add_library(PkgGLIBMM INTERFACE)
    target_include_directories(PkgGLIBMM INTERFACE ${GLIBMM_INCLUDE_DIRS})
    target_link_libraries(PkgGLIBMM INTERFACE ${GLIBMM_LIBRARIES})
    target_compile_options(PkgGLIBMM INTERFACE ${GLIBMM_CFLAGS_OTHER})
  endif()

  pkg_check_modules(FLAC REQUIRED flac)
  add_library(PkgFLAC INTERFACE)
  target_include_directories(PkgFLAC INTERFACE ${FLAC_INCLUDE_DIRS})
  target_link_libraries(PkgFLAC INTERFACE ${FLAC_LIBRARIES})

  pkg_check_modules(ALAC REQUIRED alac)
  add_library(PkgALAC INTERFACE)
  target_include_directories(PkgALAC INTERFACE ${ALAC_INCLUDE_DIRS})
  target_link_libraries(PkgALAC INTERFACE ${ALAC_LIBRARIES})

  pkg_check_modules(MPG123 REQUIRED IMPORTED_TARGET libmpg123)
  add_library(PkgMPG123 INTERFACE)
  target_link_libraries(PkgMPG123 INTERFACE PkgConfig::MPG123)

  pkg_check_modules(FDKAAC REQUIRED IMPORTED_TARGET fdk-aac)
  add_library(PkgFDKAAC INTERFACE)
  target_link_libraries(PkgFDKAAC INTERFACE PkgConfig::FDKAAC)

  pkg_check_modules(PIPEWIRE libpipewire-0.3)
  if(PIPEWIRE_FOUND)
    add_library(PkgPipeWire INTERFACE)
    target_include_directories(PkgPipeWire SYSTEM INTERFACE ${PIPEWIRE_INCLUDE_DIRS})
    target_link_libraries(PkgPipeWire INTERFACE ${PIPEWIRE_LIBRARIES})
  endif()

  pkg_check_modules(ALSA alsa)
  if(ALSA_FOUND)
    add_library(PkgALSA INTERFACE)
    target_include_directories(PkgALSA INTERFACE ${ALSA_INCLUDE_DIRS})
    target_link_libraries(PkgALSA INTERFACE ${ALSA_LIBRARIES})
  endif()

  pkg_check_modules(UDEV REQUIRED libudev)
  add_library(PkgUDEV INTERFACE)
  target_include_directories(PkgUDEV INTERFACE ${UDEV_INCLUDE_DIRS})
  target_link_libraries(PkgUDEV INTERFACE ${UDEV_LIBRARIES})

  pkg_check_modules(LMDB REQUIRED lmdb)

  pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)
  add_library(PkgXXHash INTERFACE)
  target_link_libraries(PkgXXHash INTERFACE PkgConfig::XXHASH)
endif()
