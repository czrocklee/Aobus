// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/Device.h>

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::winui::test
{
  /// A discard route for real UI availability without an audio endpoint or playback.
  audio::Device silentAudioDevice();
  void addSilentAudioOutput(rt::AppRuntime& runtime);
} // namespace ao::winui::test
