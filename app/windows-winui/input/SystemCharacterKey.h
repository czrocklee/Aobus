// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/winui/input/KeyChordAccelerator.h>

namespace ao::winui
{
  /**
   * @brief Resolves characters against the keyboard layout this thread is using.
   *
   * The portable translation table in `KeyChordAccelerator` assumes a US
   * layout, because Windows names its punctuation keys by position and only the
   * system knows which position carries which character. A shell installing
   * real accelerators asks the system instead.
   *
   * The answer is taken once, when the accelerators are installed. A layout
   * switched during the session is not picked up until they are installed
   * again.
   */
  CharacterKeyResolver systemCharacterKeyResolver();
} // namespace ao::winui
