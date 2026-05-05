# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2025 Aobus Contributors
#
# External dependency discovery and INTERFACE wrapper targets.
# All wrapper targets use the Pkg prefix to avoid collisions with
# pkg-config variable prefixes (e.g. PkgGTK4 instead of GTK4).

find_package(PkgConfig REQUIRED)
find_package(Catch2 3 REQUIRED)
find_package(Boost REQUIRED COMPONENTS headers)
find_package(CLI11 CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(yaml-cpp REQUIRED)

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
