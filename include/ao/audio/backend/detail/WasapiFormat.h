// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Format.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// mmreg.h assumes the base Win32 types and pointer macros from windows.h.
// clang-format off
#include <windows.h>
#include <mmreg.h>
// clang-format on

#include <optional>

namespace ao::audio::backend::detail
{
  /**
   * @brief Converts a PCM/IEEE-float Windows wave format to the graph format model.
   *
   * Unknown encodings and dimensions that cannot be represented by Format stay
   * unknown so the quality graph does not claim a verified endpoint format.
   */
  std::optional<Format> formatFromWaveFormat(WAVEFORMATEX const& wave) noexcept;
} // namespace ao::audio::backend::detail
