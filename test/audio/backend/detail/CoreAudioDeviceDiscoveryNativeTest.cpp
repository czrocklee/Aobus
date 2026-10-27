// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/CoreAudioDeviceDiscovery.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("CoreAudioDeviceDiscovery - native snapshot contains only stable concrete identities")
  {
    auto const devices = enumerateCoreAudioOutputDevices();

    if (devices.empty())
    {
      SKIP("macOS host has no live Core Audio output device");
    }

    std::size_t defaultCount = 0;

    for (auto const& device : devices)
    {
      CHECK_FALSE(device.id.empty());
      CHECK_FALSE(device.displayName.empty());
      CHECK(device.backendId == kBackendCoreAudio);
      defaultCount += device.isDefault ? 1U : 0U;

      auto const resolvedRes = coreAudioOutputDeviceId(device.id.raw());
      CHECK(resolvedRes);
    }

    CHECK(defaultCount <= 1U);
  }
} // namespace ao::audio::backend::detail::test
