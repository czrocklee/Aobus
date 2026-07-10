// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Exception.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Property.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/backend/WasapiProvider.h>
#include <ao/audio/backend/detail/WasapiProviderMonitorHooks.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <semaphore>
#include <thread>
#include <vector>

namespace ao::audio::backend::test
{
  TEST_CASE("WasapiProvider - exposes shared-mode wiring without a render endpoint", "[audio][unit][wasapi][provider]")
  {
    auto snapshotMutex = std::mutex{};
    auto deviceSnapshot = std::vector<Device>{};
    std::size_t deviceSnapshotCount = 0;
    auto provider = WasapiProvider{};
    auto const status = provider.status();

    CHECK(status.descriptor.id == kBackendWasapi);
    CHECK(status.descriptor.name == "WASAPI");
    CHECK(status.descriptor.description == "Windows Audio Session API");
    REQUIRE(status.descriptor.supportedProfiles.size() == 1);
    CHECK(status.descriptor.supportedProfiles.front().id == kProfileShared);

    auto deviceSub = provider.subscribeDevices(
      [&](std::vector<Device> const& devices)
      {
        auto const lock = std::scoped_lock{snapshotMutex};
        deviceSnapshot = devices;
        ++deviceSnapshotCount;
      });

    auto graph = flow::Graph{};
    std::size_t graphUpdateCount = 0;
    auto graphSub = provider.subscribeGraph("synthetic-endpoint",
                                            [&](flow::Graph const& nextGraph)
                                            {
                                              graph = nextGraph;
                                              ++graphUpdateCount;
                                            });
    auto backendPtr =
      provider.createBackend(Device{.id = DeviceId{"synthetic-endpoint"}, .backendId = kBackendWasapi}, kProfileShared);

    REQUIRE(backendPtr);
    CHECK(backendPtr->backendId() == kBackendWasapi);
    CHECK(backendPtr->profileId() == kProfileShared);
    CHECK(graphUpdateCount == 1);
    REQUIRE(backendPtr->set(props::kVolume, 0.5F));
    CHECK(graphUpdateCount == 2);
    REQUIRE(graph.nodes.size() == 2);
    CHECK_FALSE(graph.nodes[1].softwareVolumeNotUnity);
    CHECK(graph.nodes[1].maxSoftwareGain == 1.0F);

    provider.shutdown();
    provider.shutdown();

    {
      auto const lock = std::scoped_lock{snapshotMutex};
      CHECK(deviceSnapshotCount >= 1);
    }

    CHECK(provider.status().devices.empty());

    std::size_t lateDeviceCalls = 0;
    auto lateDeviceSub = provider.subscribeDevices([&](std::vector<Device> const&) { ++lateDeviceCalls; });
    CHECK(lateDeviceCalls == 0);
    CHECK_FALSE(lateDeviceSub);

    CHECK(graphUpdateCount == 3);
    CHECK(graph.nodes.empty());
    CHECK(graph.connections.empty());

    REQUIRE(backendPtr->set(props::kVolume, 0.25F));
    CHECK(graphUpdateCount == 3);

    backendPtr->close();
    CHECK(graphUpdateCount == 3);
    CHECK(graph.nodes.empty());
    CHECK(graph.connections.empty());
  }

  TEST_CASE("WasapiProvider - device callback may destroy provider safely", "[audio][regression][wasapi][provider]")
  {
    auto providerPtr = std::make_unique<WasapiProvider>();
    std::size_t callbackCount = 0;

    auto sub = providerPtr->subscribeDevices(
      [&](std::vector<Device> const&)
      {
        ++callbackCount;
        providerPtr.reset();
      });

    CHECK(callbackCount == 1);
    CHECK_FALSE(providerPtr);
    CHECK_FALSE(sub);
    sub.reset();
  }

  TEST_CASE("WasapiProvider - monitor callback may destroy provider on its own thread",
            "[audio][regression][wasapi][provider]")
  {
    auto monitorExited = std::binary_semaphore{0};
    auto monitorStateDestroyed = std::binary_semaphore{0};
    auto hooksPtr = std::make_shared<detail::WasapiProviderMonitorHooks>();
    hooksPtr->enumerateDevices = []
    { return std::vector<Device>{{.id = DeviceId{"synthetic-endpoint"}, .backendId = kBackendWasapi}}; };
    hooksPtr->onMonitorExit = [&] { monitorExited.release(); };
    hooksPtr->onMonitorStateDestroyed = [&] { monitorStateDestroyed.release(); };

    auto const callerThread = std::this_thread::get_id();
    auto callbackThread = std::thread::id{};
    auto callbackCount = std::atomic{std::size_t{0}};
    auto providerPtr = std::make_unique<WasapiProvider>(hooksPtr);
    auto sub = providerPtr->subscribeDevices(
      [&](std::vector<Device> const&)
      {
        if (callbackCount.fetch_add(1, std::memory_order_relaxed) == 0)
        {
          return; // synchronous initial snapshot
        }

        callbackThread = std::this_thread::get_id();
        providerPtr.reset();
      });
    REQUIRE(sub);
    REQUIRE(hooksPtr->requestRefresh);

    hooksPtr->requestRefresh();

    REQUIRE(monitorExited.try_acquire_for(std::chrono::seconds{5}));
    REQUIRE(monitorStateDestroyed.try_acquire_for(std::chrono::seconds{5}));
    CHECK(callbackCount.load(std::memory_order_relaxed) == 2);
    CHECK(callbackThread != callerThread);
    CHECK_FALSE(providerPtr);
    sub.reset();
  }

  TEST_CASE("WasapiProvider - initial callback destruction cannot deadlock a waiting monitor",
            "[audio][regression][wasapi][provider]")
  {
    auto callbacksReady = std::binary_semaphore{0};
    auto monitorExited = std::binary_semaphore{0};
    auto hooksPtr = std::make_shared<detail::WasapiProviderMonitorHooks>();
    hooksPtr->enumerateDevices = []
    { return std::vector<Device>{{.id = DeviceId{"synthetic-endpoint"}, .backendId = kBackendWasapi}}; };
    hooksPtr->onDeviceCallbacksReady = [&] { callbacksReady.release(); };
    hooksPtr->onMonitorExit = [&] { monitorExited.release(); };
    auto providerPtr = std::make_unique<WasapiProvider>(hooksPtr);
    std::size_t callbackCount = 0;

    auto sub = providerPtr->subscribeDevices(
      [&](std::vector<Device> const&)
      {
        ++callbackCount;
        REQUIRE(hooksPtr->requestRefresh);
        hooksPtr->requestRefresh();
        REQUIRE(callbacksReady.try_acquire_for(std::chrono::seconds{5}));
        providerPtr.reset();
      });

    CHECK(callbackCount == 1);
    CHECK_FALSE(providerPtr);
    CHECK_FALSE(sub);
    CHECK(monitorExited.try_acquire_for(std::chrono::seconds{5}));
  }

  TEST_CASE("WasapiProvider - throwing refresh callback is removed without terminating monitor",
            "[audio][regression][wasapi][provider]")
  {
    auto refreshCompleted = std::binary_semaphore{0};
    auto hooksPtr = std::make_shared<detail::WasapiProviderMonitorHooks>();
    hooksPtr->enumerateDevices = []
    { return std::vector<Device>{{.id = DeviceId{"synthetic-endpoint"}, .backendId = kBackendWasapi}}; };
    hooksPtr->onRefreshComplete = [&] { refreshCompleted.release(); };
    auto provider = WasapiProvider{hooksPtr};
    auto callbackCount = std::atomic{std::size_t{0}};
    auto sub = provider.subscribeDevices(
      [&](std::vector<Device> const&)
      {
        if (callbackCount.fetch_add(1, std::memory_order_relaxed) != 0)
        {
          throwException<Exception>("expected monitor callback failure");
        }
      });
    REQUIRE(sub);
    REQUIRE(hooksPtr->requestRefresh);

    hooksPtr->requestRefresh();
    REQUIRE(refreshCompleted.try_acquire_for(std::chrono::seconds{5}));
    CHECK(callbackCount.load(std::memory_order_relaxed) == 2);

    hooksPtr->requestRefresh();
    REQUIRE(refreshCompleted.try_acquire_for(std::chrono::seconds{5}));
    CHECK(callbackCount.load(std::memory_order_relaxed) == 2);

    sub.reset();
    provider.shutdown();
  }

  TEST_CASE("WasapiProvider - cancellation removes a device callback already copied by monitor",
            "[audio][regression][wasapi][provider]")
  {
    auto refreshCompleted = std::binary_semaphore{0};
    auto hooksPtr = std::make_shared<detail::WasapiProviderMonitorHooks>();
    hooksPtr->enumerateDevices = []
    { return std::vector<Device>{{.id = DeviceId{"synthetic-endpoint"}, .backendId = kBackendWasapi}}; };
    hooksPtr->onRefreshComplete = [&] { refreshCompleted.release(); };
    auto provider = WasapiProvider{hooksPtr};
    auto cancelSecond = std::atomic{false};
    auto firstCalls = std::atomic{std::size_t{0}};
    auto secondCalls = std::atomic{std::size_t{0}};
    auto secondSub = Subscription{};
    auto firstSub = provider.subscribeDevices(
      [&](std::vector<Device> const&)
      {
        firstCalls.fetch_add(1, std::memory_order_relaxed);

        if (cancelSecond.load(std::memory_order_acquire))
        {
          secondSub.reset();
        }
      });
    secondSub = provider.subscribeDevices([&](std::vector<Device> const&)
                                          { secondCalls.fetch_add(1, std::memory_order_relaxed); });
    REQUIRE(firstSub);
    REQUIRE(secondSub);
    cancelSecond.store(true, std::memory_order_release);
    REQUIRE(hooksPtr->requestRefresh);

    hooksPtr->requestRefresh();

    REQUIRE(refreshCompleted.try_acquire_for(std::chrono::seconds{5}));
    CHECK(firstCalls.load(std::memory_order_relaxed) == 2);
    CHECK(secondCalls.load(std::memory_order_relaxed) == 1);

    hooksPtr->requestRefresh();
    REQUIRE(refreshCompleted.try_acquire_for(std::chrono::seconds{5}));
    CHECK(firstCalls.load(std::memory_order_relaxed) == 3);
    CHECK(secondCalls.load(std::memory_order_relaxed) == 1);
    provider.shutdown();
  }

  TEST_CASE("WasapiProvider - device subscription racing shutdown receives no initial callback",
            "[audio][regression][wasapi][provider]")
  {
    auto monitorExited = std::binary_semaphore{0};
    auto startSubscription = std::binary_semaphore{0};
    auto subscriptionAttempted = std::binary_semaphore{0};
    auto racedCalls = std::atomic{std::size_t{0}};
    auto hooksPtr = std::make_shared<detail::WasapiProviderMonitorHooks>();
    hooksPtr->enumerateDevices = []
    { return std::vector<Device>{{.id = DeviceId{"synthetic-endpoint"}, .backendId = kBackendWasapi}}; };
    hooksPtr->onMonitorExit = [&] { monitorExited.release(); };
    auto provider = WasapiProvider{hooksPtr};
    auto racedSub = Subscription{};
    auto subscribeThread =
      std::jthread{[&]
                   {
                     startSubscription.acquire();
                     subscriptionAttempted.release();
                     racedSub = provider.subscribeDevices([&](std::vector<Device> const&)
                                                          { racedCalls.fetch_add(1, std::memory_order_relaxed); });
                   }};
    bool coordinateShutdown = false;
    bool shutdownCoordinated = false;
    auto coordinatorSub = provider.subscribeDevices(
      [&](std::vector<Device> const&)
      {
        if (!coordinateShutdown || shutdownCoordinated)
        {
          return;
        }

        shutdownCoordinated = true;
        startSubscription.release();
        subscriptionAttempted.acquire();
        provider.shutdown();
      });
    coordinateShutdown = true;
    REQUIRE(hooksPtr->requestRefresh);

    hooksPtr->requestRefresh();

    REQUIRE(monitorExited.try_acquire_for(std::chrono::seconds{5}));
    subscribeThread.join();
    CHECK(racedCalls.load(std::memory_order_relaxed) == 0);
    CHECK_FALSE(racedSub);
  }

  TEST_CASE("WasapiProvider - device subscription may outlive provider", "[audio][regression][wasapi][provider]")
  {
    auto sub = Subscription{};

    {
      auto provider = WasapiProvider{};
      sub = provider.subscribeDevices([](std::vector<Device> const&) {});
    }

    sub.reset();
    CHECK_FALSE(sub);
  }

  TEST_CASE("WasapiProvider - backend cannot republish a route after provider destruction",
            "[audio][regression][wasapi][provider]")
  {
    auto providerPtr = std::make_unique<WasapiProvider>();
    auto graph = flow::Graph{};
    std::size_t graphUpdateCount = 0;
    auto graphSub = providerPtr->subscribeGraph("synthetic-endpoint",
                                                [&](flow::Graph const& nextGraph)
                                                {
                                                  graph = nextGraph;
                                                  ++graphUpdateCount;
                                                });
    auto backendPtr = providerPtr->createBackend(
      Device{.id = DeviceId{"synthetic-endpoint"}, .backendId = kBackendWasapi}, kProfileShared);
    REQUIRE(backendPtr);
    REQUIRE(backendPtr->set(props::kVolume, 0.5F));
    REQUIRE(graphUpdateCount == 2);

    providerPtr.reset();

    CHECK(graphUpdateCount == 3);
    CHECK(graph.nodes.empty());
    CHECK(graph.connections.empty());

    REQUIRE(backendPtr->set(props::kVolume, 0.25F));
    backendPtr->close();
    CHECK(graphUpdateCount == 3);

    graphSub.reset();
  }
} // namespace ao::audio::backend::test
