// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/BackendDeviceRegistry.h"

#include <ao/audio/Device.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("BackendDeviceRegistry - publishes snapshots and honors cancellation", "[audio][unit][device-registry]")
  {
    auto registry = BackendDeviceRegistry{};
    std::size_t calls = 0;
    auto snapshot = std::vector<Device>{};
    auto sub = registry.subscribe(
      [&](std::vector<Device> const& devices)
      {
        ++calls;
        snapshot = devices;
      });
    REQUIRE(sub);
    CHECK(calls == 1U);
    CHECK(snapshot.empty());

    auto const firstDevices = std::vector<Device>{{.id = DeviceId{"uid-a"}, .displayName = "Output A"}};
    registry.publish(firstDevices);
    REQUIRE(calls == 2U);
    CHECK(snapshot == firstDevices);
    CHECK(registry.snapshot() == firstDevices);

    sub.reset();
    auto const nextDevices = std::vector<Device>{{.id = DeviceId{"uid-b"}, .displayName = "Output B"}};
    registry.publish(nextDevices);
    CHECK(calls == 2U);
    CHECK(snapshot == firstDevices);
    CHECK(registry.snapshot() == nextDevices);
  }

  TEST_CASE("BackendDeviceRegistry - shutdown closes admission and clears status", "[audio][unit][device-registry]")
  {
    auto registry = BackendDeviceRegistry{};
    auto const devices = std::vector<Device>{{.id = DeviceId{"uid-a"}}};
    registry.publish(devices);
    REQUIRE(registry.snapshot() == devices);
    std::size_t calls = 0;
    auto initialSnapshot = std::vector<Device>{};
    auto sub = registry.subscribe(
      [&](std::vector<Device> const& current)
      {
        initialSnapshot = current;
        ++calls;
      });
    REQUIRE(sub);
    CHECK(calls == 1U);
    CHECK(initialSnapshot == devices);

    registry.shutdown();
    registry.shutdown();
    registry.publish({{.id = DeviceId{"uid-b"}}});

    CHECK(registry.snapshot().empty());
    CHECK(calls == 1U);
    CHECK_FALSE(registry.subscribe([&](std::vector<Device> const&) { ++calls; }));
    sub.reset();
  }

  TEST_CASE("BackendDeviceRegistry - one callback may cancel another copied subscriber",
            "[audio][unit][device-registry][concurrency]")
  {
    auto registry = BackendDeviceRegistry{};
    std::size_t firstCalls = 0;
    std::size_t secondCalls = 0;
    bool cancelSecond = false;
    auto secondSub = utility::ScopedRegistration{};
    auto firstSub = registry.subscribe(
      [&](std::vector<Device> const&)
      {
        ++firstCalls;

        if (cancelSecond)
        {
          secondSub.reset();
        }
      });
    secondSub = registry.subscribe([&](std::vector<Device> const&) { ++secondCalls; });
    cancelSecond = true;

    registry.publish({});

    CHECK(firstCalls == 2U);
    CHECK(secondCalls == 1U);
  }
} // namespace ao::audio::backend::detail::test
