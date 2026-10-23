// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "input/SystemCharacterKey.h"

#include "pch.h"
#include <ao/winui/input/KeyChordAccelerator.h>

#include <cstdint>
#include <optional>

namespace ao::winui
{
  namespace
  {
    /// Takes one byte out of the word `VkKeyScanExW` packs its answer into.
    constexpr std::uint32_t kScanByteMask = 0xFF;
    /// The shift-state bits `VkKeyScanExW` reports in its high byte.
    constexpr std::uint32_t kScanShift = 1;
    constexpr std::uint32_t kScanControl = 2;
    constexpr std::uint32_t kScanAlt = 4;
  } // namespace

  CharacterKeyResolver systemCharacterKeyResolver()
  {
    return [](char const character) -> std::optional<CharacterKey>
    {
      // The layout of the calling thread, which is the UI thread here.
      auto const scan = ::VkKeyScanExW(static_cast<WCHAR>(character), ::GetKeyboardLayout(0));

      if (scan == -1)
      {
        return std::nullopt;
      }

      auto const state = static_cast<std::uint32_t>((static_cast<std::uint32_t>(scan) >> 8U) & kScanByteMask);

      if ((state & (kScanControl | kScanAlt)) != 0)
      {
        // Reachable only through AltGr or a Ctrl composition. A keyboard
        // accelerator names one key plus modifiers, so it cannot ask for a
        // character that is itself produced by modifiers; installing the base
        // key would bind a different keystroke than the user wrote.
        return std::nullopt;
      }

      return CharacterKey{
        .virtualKey = static_cast<std::uint32_t>(scan) & kScanByteMask,
        .needsShift = (state & kScanShift) != 0,
      };
    };
  }
} // namespace ao::winui
