// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::tui
{
  void registerPlatformAudioBackends(rt::AppRuntime& runtime);
} // namespace ao::tui
