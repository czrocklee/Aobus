// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/CoreAudioDeviceDiscovery.h"

#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <utility>
#include <vector>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("CoreAudioDeviceDiscovery - marks and orders the concrete system default", "[audio][unit][coreaudio]")
  {
    auto devices = std::vector<Device>{
      {.id = DeviceId{"uid-z"}, .displayName = "Zeta", .isDefault = true, .backendId = kBackendCoreAudio},
      {.id = DeviceId{"uid-b"}, .displayName = "Beta", .backendId = kBackendCoreAudio},
      {.id = DeviceId{"uid-a"}, .displayName = "Alpha", .backendId = kBackendCoreAudio}};

    auto ordered = orderCoreAudioDevices(std::move(devices), "uid-b");

    REQUIRE(ordered.size() == 3U);
    CHECK(ordered[0].id == "uid-b");
    CHECK(ordered[0].isDefault);
    CHECK(ordered[1].id == "uid-a");
    CHECK_FALSE(ordered[1].isDefault);
    CHECK(ordered[2].id == "uid-z");
    CHECK_FALSE(ordered[2].isDefault);
  }

  TEST_CASE("CoreAudioDeviceDiscovery - native snapshot contains only stable concrete identities",
            "[audio][integration][coreaudio]")
  {
    auto const devices = enumerateCoreAudioOutputDevices();
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

  TEST_CASE("CoreAudioDeviceDiscovery - UID resolution rejects invalid and absent identities",
            "[audio][unit][coreaudio]")
  {
    auto const invalidRes = coreAudioOutputDeviceId(std::string{"\xC3\x28", 2U});
    REQUIRE_FALSE(invalidRes);
    CHECK(invalidRes.error().code == Error::Code::InvalidInput);

    auto const missingRes = coreAudioOutputDeviceId("org.aobus.missing-output-device");
    REQUIRE_FALSE(missingRes);
    CHECK(missingRes.error().code == Error::Code::DeviceNotFound);
  }
} // namespace ao::audio::backend::detail::test
