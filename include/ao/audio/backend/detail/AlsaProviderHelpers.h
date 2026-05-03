// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/audio/backend/detail/AudioBackendShared.h>
#include <string>
#include <vector>

namespace ao::audio::backend::detail
{
  /**
   * @brief Queries hardware capabilities for a specific ALSA device name (e.g., "hw:0,0").
   */
  ao::audio::DeviceCapabilities queryAlsaDeviceCapabilities(std::string const& deviceName);

  /**

   * @brief Enumerates all available ALSA playback devices (physical cards).
   * Returns a list of devices in both 'plughw' and 'hw' variants.
   */
  std::vector<ao::audio::Device> doAlsaEnumerate();
} // namespace ao::audio::backend::detail
