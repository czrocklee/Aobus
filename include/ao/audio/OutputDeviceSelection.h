// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>

namespace ao::audio
{
  /** Stable identity of one requested or selected output route. */
  struct OutputDeviceSelection final
  {
    BackendId backendId{};
    DeviceId deviceId{};
    ProfileId profileId{};

    bool operator==(OutputDeviceSelection const&) const = default;
  };
} // namespace ao::audio
