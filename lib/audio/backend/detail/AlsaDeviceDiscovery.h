// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Device.h>

#include <vector>

namespace ao::audio::backend::detail
{
  /**
   * @brief Enumerates direct ALSA hardware playback devices for physical cards.
   *
   * Plugin PCMs such as `plughw` are omitted because they may convert the
   * client stream and cannot satisfy the exclusive backend's raw-mode contract.
   */
  std::vector<Device> enumerateAlsaPlaybackDevices();
} // namespace ao::audio::backend::detail
