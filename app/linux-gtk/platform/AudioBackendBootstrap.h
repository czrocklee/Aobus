// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::gtk
{
  /**
   * Registers platform-specific audio backend providers (e.g., PipeWire, ALSA)
   * with the application runtime.
   */
  void registerPlatformAudioBackends(rt::AppRuntime& runtime);
} // namespace ao::gtk
