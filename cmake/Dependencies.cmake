# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2025 Aobus Contributors
#
# External dependency discovery and INTERFACE wrapper targets.
# All wrapper targets use the Pkg prefix to avoid collisions with
# pkg-config variable prefixes (e.g. PkgGTK4 instead of GTK4).

find_package(PkgConfig REQUIRED)
find_package(Catch2 3 REQUIRED)
find_package(Boost REQUIRED COMPONENTS headers process filesystem)
find_package(CLI11 CONFIG REQUIRED)
find_package(ftxui CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(ryml REQUIRED)
find_package(c4core REQUIRED)

add_library(PkgRapidYaml INTERFACE)
target_link_libraries(PkgRapidYaml INTERFACE ryml::ryml c4core::c4core)

# ── spdlog ABI guard ────────────────────────────────────────────────────────
# Aobus compiles spdlog call sites with SPDLOG_USE_STD_FORMAT.  If CMake reuses
# a stale cache entry that points at the default fmt-backed spdlog package, the
# build can succeed but fail at runtime with missing log_msg(std::string_view)
# symbols.  Fail during configure instead.
get_target_property(AOBUS_SPDLOG_COMPILE_DEFINITIONS spdlog::spdlog INTERFACE_COMPILE_DEFINITIONS)
if(NOT AOBUS_SPDLOG_COMPILE_DEFINITIONS)
  set(AOBUS_SPDLOG_COMPILE_DEFINITIONS "")
endif()

if(NOT SPDLOG_USE_STD_FORMAT IN_LIST AOBUS_SPDLOG_COMPILE_DEFINITIONS)
  message(FATAL_ERROR
    "Aobus requires spdlog built with SPDLOG_USE_STD_FORMAT, but the located "
    "spdlog package does not advertise that ABI. spdlog_DIR='${spdlog_DIR}', "
    "INTERFACE_COMPILE_DEFINITIONS='${AOBUS_SPDLOG_COMPILE_DEFINITIONS}'. "
    "Remove the build directory or CMakeCache.txt, then reconfigure inside nix-shell.")
endif()

if(SPDLOG_FMT_EXTERNAL IN_LIST AOBUS_SPDLOG_COMPILE_DEFINITIONS)
  message(FATAL_ERROR
    "Aobus requires std::format-backed spdlog, but the located spdlog target "
    "uses SPDLOG_FMT_EXTERNAL. spdlog_DIR='${spdlog_DIR}'. Remove the build "
    "directory or CMakeCache.txt, then reconfigure inside nix-shell.")
endif()

find_program(GLIB_COMPILE_RESOURCES_EXECUTABLE glib-compile-resources REQUIRED)
find_program(GPERF_EXECUTABLE gperf REQUIRED)

# ── GTK4 ────────────────────────────────────────────────────────────────────
pkg_check_modules(GTK4 REQUIRED gtk4)
add_library(PkgGTK4 INTERFACE)
target_include_directories(PkgGTK4 INTERFACE ${GTK4_INCLUDE_DIRS})
target_link_libraries(PkgGTK4 INTERFACE ${GTK4_LIBRARIES})
target_compile_options(PkgGTK4 INTERFACE ${GTK4_CFLAGS_OTHER})

# ── gtkmm ───────────────────────────────────────────────────────────────────
pkg_check_modules(GTKMM REQUIRED gtkmm-4.0)
add_library(PkgGTKMM INTERFACE)
target_include_directories(PkgGTKMM INTERFACE ${GTKMM_INCLUDE_DIRS})
target_link_libraries(PkgGTKMM INTERFACE ${GTKMM_LIBRARIES})
target_compile_options(PkgGTKMM INTERFACE ${GTKMM_CFLAGS_OTHER})

# ── glibmm ──────────────────────────────────────────────────────────────────
pkg_check_modules(GLIBMM REQUIRED glibmm-2.68)
add_library(PkgGLIBMM INTERFACE)
target_include_directories(PkgGLIBMM INTERFACE ${GLIBMM_INCLUDE_DIRS})
target_link_libraries(PkgGLIBMM INTERFACE ${GLIBMM_LIBRARIES})
target_compile_options(PkgGLIBMM INTERFACE ${GLIBMM_CFLAGS_OTHER})

# ── gdk-pixbuf ──────────────────────────────────────────────────────────────
pkg_check_modules(GDKPIXBUF REQUIRED gdk-pixbuf-2.0)
add_library(PkgGDKPIXBUF INTERFACE)
target_include_directories(PkgGDKPIXBUF INTERFACE ${GDKPIXBUF_INCLUDE_DIRS})
target_link_libraries(PkgGDKPIXBUF INTERFACE ${GDKPIXBUF_LIBRARIES})
target_compile_options(PkgGDKPIXBUF INTERFACE ${GDKPIXBUF_CFLAGS_OTHER})

# ── FLAC ────────────────────────────────────────────────────────────────────
pkg_check_modules(FLAC REQUIRED flac)
add_library(PkgFLAC INTERFACE)
target_include_directories(PkgFLAC INTERFACE ${FLAC_INCLUDE_DIRS})
target_link_libraries(PkgFLAC INTERFACE ${FLAC_LIBRARIES})
message(STATUS "FLAC found")

# ── ALAC ────────────────────────────────────────────────────────────────────
pkg_check_modules(ALAC REQUIRED alac)
add_library(PkgALAC INTERFACE)
target_include_directories(PkgALAC INTERFACE ${ALAC_INCLUDE_DIRS})
target_link_libraries(PkgALAC INTERFACE ${ALAC_LIBRARIES})
message(STATUS "ALAC found")

# ── MP3 (mpg123) ────────────────────────────────────────────────────────────
pkg_check_modules(MPG123 REQUIRED IMPORTED_TARGET libmpg123)
add_library(PkgMPG123 INTERFACE)
target_link_libraries(PkgMPG123 INTERFACE PkgConfig::MPG123)
message(STATUS "mpg123 found")

# ── AAC (FDK-AAC) ───────────────────────────────────────────────────────────
pkg_check_modules(FDKAAC REQUIRED IMPORTED_TARGET fdk-aac)
add_library(PkgFDKAAC INTERFACE)
target_link_libraries(PkgFDKAAC INTERFACE PkgConfig::FDKAAC)
message(STATUS "FDK-AAC found")

# ── PipeWire (optional) ────────────────────────────────────────────────────
pkg_check_modules(PIPEWIRE libpipewire-0.3)
if(PIPEWIRE_FOUND)
  add_library(PkgPipeWire INTERFACE)
  target_include_directories(PkgPipeWire SYSTEM INTERFACE ${PIPEWIRE_INCLUDE_DIRS})
  target_link_libraries(PkgPipeWire INTERFACE ${PIPEWIRE_LIBRARIES})
  message(STATUS "PipeWire found: audio backend enabled")
endif()

# ── ALSA (optional) ────────────────────────────────────────────────────────
pkg_check_modules(ALSA alsa)
if(ALSA_FOUND)
  add_library(PkgALSA INTERFACE)
  target_include_directories(PkgALSA INTERFACE ${ALSA_INCLUDE_DIRS})
  target_link_libraries(PkgALSA INTERFACE ${ALSA_LIBRARIES})
  message(STATUS "ALSA found: ALSA exclusive backend enabled")
endif()

# ── libudev ─────────────────────────────────────────────────────────────────
pkg_check_modules(UDEV REQUIRED libudev)
add_library(PkgUDEV INTERFACE)
target_include_directories(PkgUDEV INTERFACE ${UDEV_INCLUDE_DIRS})
target_link_libraries(PkgUDEV INTERFACE ${UDEV_LIBRARIES})
message(STATUS "libudev found and enabled")

# ── lmdb ────────────────────────────────────────────────────────────────────
pkg_check_modules(LMDB REQUIRED lmdb)

# ── mimalloc ────────────────────────────────────────────────────────────────
find_package(mimalloc REQUIRED)

# ── FakeIt (Header-only Mocking Framework for tests) ────────────────────────
find_package(FakeIt CONFIG REQUIRED)
