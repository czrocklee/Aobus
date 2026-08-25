// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/BackendDeviceRegistry.h"

#include <ao/audio/Device.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("BackendDeviceRegistry - publishes snapshots and honors cancellation", "[audio][unit][device-registry]")
  {
    auto registry = BackendDeviceRegistry{};
    auto calls = std::size_t{0};
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

    registry.publish({{.id = DeviceId{"uid-a"}, .displayName = "Output A"}});
    REQUIRE(calls == 2U);
    REQUIRE(snapshot.size() == 1U);
    CHECK(snapshot.front().id == "uid-a");

    sub.reset();
    registry.publish({{.id = DeviceId{"uid-b"}, .displayName = "Output B"}});
    CHECK(calls == 2U);
  }

  TEST_CASE("BackendDeviceRegistry - shutdown closes admission and clears status",
            "[audio][unit][device-registry]")
  {
    auto registry = BackendDeviceRegistry{};
    registry.publish({{.id = DeviceId{"uid-a"}}});
    auto calls = std::size_t{0};
    auto sub = registry.subscribe([&](std::vector<Device> const&) { ++calls; });
    REQUIRE(sub);
    CHECK(calls == 1U);

    registry.shutdown();
    registry.shutdown();
    registry.publish({{.id = DeviceId{"uid-b"}}});

    CHECK(registry.snapshot().empty());
    CHECK(calls == 1U);
    CHECK_FALSE(registry.subscribe([&](std::vector<Device> const&) { ++calls; }));
    sub.reset();
  }

  TEST_CASE("BackendDeviceRegistry - one callback may cancel another copied subscriber",
            "[audio][unit][device-registry]")
  {
    auto registry = BackendDeviceRegistry{};
    auto firstCalls = std::size_t{0};
    auto secondCalls = std::size_t{0};
    auto cancelSecond = false;
    auto secondSub = Subscription{};
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
