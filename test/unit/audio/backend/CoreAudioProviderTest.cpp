// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/CoreAudioProvider.h"

#include "lib/audio/backend/detail/CoreAudioProviderMonitorHooks.h"
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <vector>

namespace ao::audio::backend::test
{
  constexpr auto kWaitTimeout = std::chrono::seconds{5};

  TEST_CASE("CoreAudioProvider - exposes shared mode and concrete native devices", "[audio][unit][coreaudio][provider]")
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

  TEST_CASE("CoreAudioProvider - reconciles device state after listener installation",
            "[audio][regression][coreaudio][provider]")
  {
    auto enumerateCount = std::atomic{std::size_t{0U}};
    auto hooksPtr = std::make_shared<detail::CoreAudioProviderMonitorHooks>();
    hooksPtr->enumerateDevices = [&]
    {
      auto const count = enumerateCount.fetch_add(1U, std::memory_order_relaxed);
      return std::vector<Device>{{.id = DeviceId{count == 0U ? "uid-before" : "uid-after"},
                                  .displayName = "Synthetic Output",
                                  .backendId = kBackendCoreAudio}};
    };

    auto provider = CoreAudioProvider{hooksPtr};
    auto const status = provider.status();

    CHECK(enumerateCount.load(std::memory_order_relaxed) == 2U);
    REQUIRE(status.devices.size() == 1U);
    CHECK(status.devices.front().id == "uid-after");
  }

  TEST_CASE("CoreAudioProvider - startup failure retires the installed monitor",
            "[audio][regression][coreaudio][provider]")
  {
    auto enumerateCount = std::atomic{std::size_t{0U}};
    auto monitorStateDestroyed = std::binary_semaphore{0};
    auto hooksPtr = std::make_shared<detail::CoreAudioProviderMonitorHooks>();
    hooksPtr->enumerateDevices = [&]
    {
      if (enumerateCount.fetch_add(1U, std::memory_order_relaxed) != 0U)
      {
        throw std::runtime_error{"synthetic Core Audio startup failure"};
      }

      return std::vector<Device>{};
    };
    hooksPtr->onMonitorStateDestroyed = [&] { monitorStateDestroyed.release(); };

    CHECK_THROWS_AS(CoreAudioProvider{hooksPtr}, std::runtime_error);
    CHECK(enumerateCount.load(std::memory_order_relaxed) == 2U);
    CHECK(monitorStateDestroyed.try_acquire());
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
      return std::vector<Device>{{.id = DeviceId{count < 2U ? "uid-a" : "uid-b"},
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
    REQUIRE(refreshComplete.try_acquire_for(kWaitTimeout));

    REQUIRE(snapshots.size() == 2U);
    REQUIRE(snapshots[0].size() == 1U);
    REQUIRE(snapshots[1].size() == 1U);
    CHECK(snapshots[0][0].id == "uid-a");
    CHECK(snapshots[1][0].id == "uid-b");
    CHECK(enumerateCount.load(std::memory_order_relaxed) == 3U);
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
        return std::vector<Device>{
          {.id = DeviceId{"uid-a"}, .displayName = "Synthetic Output", .backendId = kBackendCoreAudio}};
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

  TEST_CASE("CoreAudioProvider - nested provider callback may destroy the outer provider",
            "[audio][regression][coreaudio][concurrency]")
  {
    auto makeHooks = []
    {
      auto hooksPtr = std::make_shared<detail::CoreAudioProviderMonitorHooks>();
      hooksPtr->enumerateDevices = []
      {
        return std::vector<Device>{
          {.id = DeviceId{"uid-a"}, .displayName = "Synthetic Output", .backendId = kBackendCoreAudio}};
      };
      return hooksPtr;
    };
    auto outerMonitorExited = std::binary_semaphore{0};
    auto outerStateDestroyed = std::binary_semaphore{0};
    auto outerHooksPtr = makeHooks();
    outerHooksPtr->onMonitorExit = [&] { outerMonitorExited.release(); };
    outerHooksPtr->onMonitorStateDestroyed = [&] { outerStateDestroyed.release(); };
    auto outerProviderPtr = std::make_unique<CoreAudioProvider>(outerHooksPtr);
    auto innerProvider = CoreAudioProvider{makeHooks()};
    auto innerSub = Subscription{};
    std::size_t outerCallbackCount = 0U;
    std::size_t innerCallbackCount = 0U;

    auto outerSub = outerProviderPtr->subscribeDevices(
      [&](std::vector<Device> const&)
      {
        ++outerCallbackCount;
        innerSub = innerProvider.subscribeDevices(
          [&](std::vector<Device> const&)
          {
            ++innerCallbackCount;
            outerProviderPtr.reset();
          });
      });

    CHECK_FALSE(outerProviderPtr);
    CHECK_FALSE(outerSub);
    REQUIRE(innerSub);
    CHECK(outerCallbackCount == 1U);
    CHECK(innerCallbackCount == 1U);
    REQUIRE(outerMonitorExited.try_acquire_for(kWaitTimeout));
    REQUIRE(outerStateDestroyed.try_acquire_for(kWaitTimeout));
    innerSub.reset();
  }

  TEST_CASE("CoreAudioProvider - device observer may destroy provider on monitor thread",
            "[audio][regression][coreaudio][concurrency]")
  {
    auto monitorExited = std::binary_semaphore{0};
    auto monitorStateDestroyed = std::binary_semaphore{0};
    auto hooksPtr = std::make_shared<detail::CoreAudioProviderMonitorHooks>();
    hooksPtr->enumerateDevices = [] { return std::vector<Device>{}; };
    hooksPtr->onMonitorExit = [&] { monitorExited.release(); };
    hooksPtr->onMonitorStateDestroyed = [&] { monitorStateDestroyed.release(); };

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
    REQUIRE(monitorExited.try_acquire_for(kWaitTimeout));
    REQUIRE(monitorStateDestroyed.try_acquire_for(kWaitTimeout));
    CHECK_FALSE(providerPtr);
    sub.reset();
  }

  TEST_CASE("CoreAudioProvider - concurrent external shutdown callers share callback quiescence",
            "[audio][regression][coreaudio][concurrency]")
  {
    auto callbackEntered = std::binary_semaphore{0};
    auto releaseCallback = std::binary_semaphore{0};
    auto shutdownStarted = std::binary_semaphore{0};
    auto shutdownWait = std::binary_semaphore{0};
    auto firstReturned = std::binary_semaphore{0};
    auto secondReturned = std::binary_semaphore{0};
    auto monitorExited = std::binary_semaphore{0};
    auto hooksPtr = std::make_shared<detail::CoreAudioProviderMonitorHooks>();
    hooksPtr->enumerateDevices = [] { return std::vector<Device>{}; };
    hooksPtr->onShutdownStarted = [&] { shutdownStarted.release(); };
    hooksPtr->onShutdownWait = [&] { shutdownWait.release(); };
    hooksPtr->onMonitorExit = [&] { monitorExited.release(); };
    auto provider = CoreAudioProvider{hooksPtr};
    auto blockCallback = std::atomic_bool{false};
    auto sub = provider.subscribeDevices(
      [&](std::vector<Device> const&)
      {
        if (blockCallback.load(std::memory_order_acquire))
        {
          callbackEntered.release();
          releaseCallback.acquire();
        }
      });
    REQUIRE(sub);
    blockCallback.store(true, std::memory_order_release);
    REQUIRE(hooksPtr->requestRefresh);
    hooksPtr->requestRefresh();
    REQUIRE(callbackEntered.try_acquire_for(kWaitTimeout));

    auto firstShutdown = std::jthread{[&]
                                      {
                                        provider.shutdown();
                                        firstReturned.release();
                                      }};
    REQUIRE(shutdownStarted.try_acquire_for(kWaitTimeout));
    auto secondShutdown = std::jthread{[&]
                                       {
                                         provider.shutdown();
                                         secondReturned.release();
                                       }};
    REQUIRE(shutdownWait.try_acquire_for(kWaitTimeout));

    CHECK_FALSE(firstReturned.try_acquire());
    CHECK_FALSE(secondReturned.try_acquire());
    releaseCallback.release();
    firstShutdown.join();
    secondShutdown.join();
    CHECK(firstReturned.try_acquire());
    CHECK(secondReturned.try_acquire());
    REQUIRE(monitorExited.try_acquire_for(kWaitTimeout));
    CHECK(provider.status().devices.empty());
    CHECK_FALSE(provider.subscribeDevices([](std::vector<Device> const&) {}));
    CHECK_FALSE(provider.subscribeGraph("uid-a", [](flow::Graph const&) {}));
  }

  TEST_CASE("CoreAudioProvider - callback shutdown returns while later external shutdown waits",
            "[audio][regression][coreaudio][concurrency]")
  {
    auto callbackShutdownReturned = std::binary_semaphore{0};
    auto releaseCallback = std::binary_semaphore{0};
    auto shutdownWait = std::binary_semaphore{0};
    auto externalReturned = std::binary_semaphore{0};
    auto monitorExited = std::binary_semaphore{0};
    auto hooksPtr = std::make_shared<detail::CoreAudioProviderMonitorHooks>();
    hooksPtr->enumerateDevices = [] { return std::vector<Device>{}; };
    hooksPtr->onShutdownWait = [&] { shutdownWait.release(); };
    hooksPtr->onMonitorExit = [&] { monitorExited.release(); };
    auto provider = CoreAudioProvider{hooksPtr};
    auto callbackCount = std::atomic{std::size_t{0U}};
    auto lateCalls = std::atomic{std::size_t{0U}};
    auto lateDeviceSub = Subscription{};
    auto lateGraphSub = Subscription{};
    auto sub = provider.subscribeDevices(
      [&](std::vector<Device> const&)
      {
        if (callbackCount.fetch_add(1U, std::memory_order_relaxed) == 0U)
        {
          return;
        }

        provider.shutdown();
        lateDeviceSub = provider.subscribeDevices([&](std::vector<Device> const&)
                                                  { lateCalls.fetch_add(1U, std::memory_order_relaxed); });
        lateGraphSub = provider.subscribeGraph(
          "uid-a", [&](flow::Graph const&) { lateCalls.fetch_add(1U, std::memory_order_relaxed); });
        callbackShutdownReturned.release();
        releaseCallback.acquire();
      });
    REQUIRE(sub);
    REQUIRE(hooksPtr->requestRefresh);

    hooksPtr->requestRefresh();
    REQUIRE(callbackShutdownReturned.try_acquire_for(kWaitTimeout));
    auto externalShutdown = std::jthread{[&]
                                         {
                                           provider.shutdown();
                                           externalReturned.release();
                                         }};
    REQUIRE(shutdownWait.try_acquire_for(kWaitTimeout));

    CHECK_FALSE(externalReturned.try_acquire());
    CHECK_FALSE(lateDeviceSub);
    CHECK_FALSE(lateGraphSub);
    CHECK(lateCalls.load(std::memory_order_relaxed) == 0U);
    releaseCallback.release();
    externalShutdown.join();
    REQUIRE(monitorExited.try_acquire_for(kWaitTimeout));
    CHECK(externalReturned.try_acquire());
    CHECK(callbackCount.load(std::memory_order_relaxed) == 2U);
    CHECK(provider.status().devices.empty());
  }

  TEST_CASE("CoreAudioProvider - initial graph observer may destroy provider and completes shared shutdown",
            "[audio][regression][coreaudio][concurrency]")
  {
    auto monitorExited = std::binary_semaphore{0};
    auto monitorStateDestroyed = std::binary_semaphore{0};
    auto hooksPtr = std::make_shared<detail::CoreAudioProviderMonitorHooks>();
    hooksPtr->enumerateDevices = [] { return std::vector<Device>{}; };
    hooksPtr->onMonitorExit = [&] { monitorExited.release(); };
    hooksPtr->onMonitorStateDestroyed = [&] { monitorStateDestroyed.release(); };
    auto providerPtr = std::make_unique<CoreAudioProvider>(hooksPtr);
    std::size_t callbackCount = 0U;

    auto graphSub = providerPtr->subscribeGraph("uid-a",
                                                [&](flow::Graph const&)
                                                {
                                                  ++callbackCount;
                                                  providerPtr.reset();
                                                });

    CHECK_FALSE(providerPtr);
    CHECK_FALSE(graphSub);
    CHECK(callbackCount == 1U);
    REQUIRE(monitorExited.try_acquire_for(kWaitTimeout));
    REQUIRE(monitorStateDestroyed.try_acquire_for(kWaitTimeout));
    graphSub.reset();
  }
} // namespace ao::audio::backend::test
