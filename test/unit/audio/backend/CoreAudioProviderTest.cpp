// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/CoreAudioProvider.h"

#include "lib/audio/backend/detail/CoreAudioProviderMonitorHooks.h"

#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Subscription.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <semaphore>
#include <vector>

namespace ao::audio::backend::test
{
  TEST_CASE("CoreAudioProvider - exposes shared mode and concrete native devices",
            "[audio][unit][coreaudio][provider]")
  {
    auto provider = CoreAudioProvider{};
    auto const status = provider.status();
    CHECK(status.descriptor.id == kBackendCoreAudio);
    REQUIRE(status.descriptor.supportedProfiles.size() == 1U);
    CHECK(status.descriptor.supportedProfiles.front().id == kProfileShared);
    for (auto const& device : status.devices)
    {
      CHECK_FALSE(device.id.empty());
      CHECK(device.backendId == kBackendCoreAudio);
    }

    provider.shutdown();
    CHECK(provider.status().devices.empty());
    CHECK_FALSE(provider.subscribeDevices([](std::vector<Device> const&) {}));
  }

  TEST_CASE("CoreAudioProvider - monitor refresh publishes a complete deterministic snapshot",
            "[audio][unit][coreaudio][provider]")
  {
    auto refreshComplete = std::binary_semaphore{0};
    auto enumerateCount = std::atomic{std::size_t{0U}};
    auto hooksPtr = std::make_shared<detail::CoreAudioProviderMonitorHooks>();
    hooksPtr->enumerateDevices = [&]
    {
      auto const count = enumerateCount.fetch_add(1U, std::memory_order_relaxed);
      return std::vector<Device>{{.id = DeviceId{count == 0U ? "uid-a" : "uid-b"},
                                  .displayName = "Synthetic Output",
                                  .backendId = kBackendCoreAudio}};
    };
    hooksPtr->onRefreshComplete = [&] { refreshComplete.release(); };
    auto provider = CoreAudioProvider{hooksPtr};
    auto snapshots = std::vector<std::vector<Device>>{};
    auto sub = provider.subscribeDevices([&](std::vector<Device> const& devices) { snapshots.push_back(devices); });
    REQUIRE(sub);
    REQUIRE(hooksPtr->requestRefresh);
    hooksPtr->requestRefresh();
    REQUIRE(refreshComplete.try_acquire_for(std::chrono::seconds{5}));

    REQUIRE(snapshots.size() == 2U);
    REQUIRE(snapshots[0].size() == 1U);
    REQUIRE(snapshots[1].size() == 1U);
    CHECK(snapshots[0][0].id == "uid-a");
    CHECK(snapshots[1][0].id == "uid-b");
    provider.shutdown();
    hooksPtr->requestRefresh();
    CHECK(snapshots.size() == 2U);
  }

  TEST_CASE("CoreAudioProvider - subscription and backend may outlive provider",
            "[audio][regression][coreaudio][provider]")
  {
    auto sub = Subscription{};
    auto backendPtr = std::unique_ptr<Backend>{};
    {
      auto hooksPtr = std::make_shared<detail::CoreAudioProviderMonitorHooks>();
      hooksPtr->enumerateDevices = []
      {
        return std::vector<Device>{{.id = DeviceId{"uid-a"},
                                    .displayName = "Synthetic Output",
                                    .backendId = kBackendCoreAudio}};
      };
      auto provider = CoreAudioProvider{hooksPtr};
      sub = provider.subscribeDevices([](std::vector<Device> const&) {});
      backendPtr = provider.createBackend(provider.status().devices.front(), kProfileShared);
    }

    REQUIRE(backendPtr);
    CHECK(backendPtr->backendId() == kBackendCoreAudio);
    CHECK(backendPtr->profileId() == kProfileShared);
    backendPtr->close();
    sub.reset();
    CHECK_FALSE(sub);
  }

  TEST_CASE("CoreAudioProvider - device observer may destroy provider on monitor thread",
            "[audio][regression][coreaudio][concurrency]")
  {
    auto monitorExited = std::binary_semaphore{0};
    auto hooksPtr = std::make_shared<detail::CoreAudioProviderMonitorHooks>();
    hooksPtr->enumerateDevices = [] { return std::vector<Device>{}; };
    hooksPtr->onMonitorExit = [&] { monitorExited.release(); };

    auto providerPtr = std::unique_ptr<CoreAudioProvider>{};
    providerPtr = std::make_unique<CoreAudioProvider>(hooksPtr);
    auto destroyOnCallback = std::atomic_bool{false};
    auto sub = providerPtr->subscribeDevices(
      [&](std::vector<Device> const&)
      {
        if (destroyOnCallback.load(std::memory_order_acquire))
        {
          providerPtr.reset();
        }
      });
    REQUIRE(sub);
    REQUIRE(hooksPtr->requestRefresh);

    destroyOnCallback.store(true, std::memory_order_release);
    hooksPtr->requestRefresh();
    REQUIRE(monitorExited.try_acquire_for(std::chrono::seconds{5}));
    CHECK_FALSE(providerPtr);
    sub.reset();
  }
} // namespace ao::audio::backend::test
