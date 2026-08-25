// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/Device.h>

#include <CoreAudio/CoreAudio.h>

#include <string_view>
#include <vector>

namespace ao::audio::backend::detail
{
  std::vector<Device> orderCoreAudioDevices(std::vector<Device> devices, std::string_view defaultDeviceUid);

  std::vector<Device> enumerateCoreAudioOutputDevices();

  Result<::AudioDeviceID> coreAudioOutputDeviceId(std::string_view deviceUid);
} // namespace ao::audio::backend::detail
