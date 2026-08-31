// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/backend/PipeWireMonitor.h"

#include "lib/audio/backend/PipeWireProvider.h"
#include "lib/audio/backend/detail/PipeWireMonitorHooks.h"
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
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ao::audio::backend::test
{
  namespace
  {
    constexpr auto kWaitTimeout = std::chrono::seconds{5};

    Device syntheticSink()
    {
      return {.id = DeviceId{"91"},
              .displayName = "Synthetic PipeWire Sink",
              .description = "Deterministic monitor sink",
              .backendId = kBackendPipeWire};
    }

    std::shared_ptr<detail::PipeWireMonitorHooks> makeMonitorHooks()
    {
      auto hooksPtr = std::make_shared<detail::PipeWireMonitorHooks>();
      hooksPtr->enumerateSinks = [] { return std::vector{syntheticSink()}; };
      hooksPtr->graphForRoute = [](std::string_view routeAnchor)
      {
        auto graph = flow::Graph{};
        graph.nodes.push_back({.id = std::string{routeAnchor}});
        return graph;
      };
      return hooksPtr;
    }
  } // namespace

  TEST_CASE("PipeWireMonitor - stop closes publication and later subscription admission",
            "[audio][unit][pipewire][monitor]")
  {
    auto hooksPtr = makeMonitorHooks();
    auto monitor = PipeWireMonitor{hooksPtr};
    auto graphDelivered = std::binary_semaphore{0};
    auto deviceCalls = std::atomic{std::size_t{0U}};
    auto graphCalls = std::atomic{std::size_t{0U}};
    auto deviceSub = monitor.subscribeDevices([&](std::vector<Device> const&)
                                              { deviceCalls.fetch_add(1U, std::memory_order_relaxed); });
    auto graphSub = monitor.subscribeGraph("42",
                                           [&](flow::Graph const&)
                                           {
                                             graphCalls.fetch_add(1U, std::memory_order_relaxed);
                                             graphDelivered.release();
                                           });
    REQUIRE(deviceSub);
    REQUIRE(graphSub);
    REQUIRE(graphDelivered.try_acquire_for(kWaitTimeout));

    monitor.stop();
    monitor.stop();
    auto const deviceCallsAfterStop = deviceCalls.load(std::memory_order_relaxed);
    auto const graphCallsAfterStop = graphCalls.load(std::memory_order_relaxed);
    monitor.refresh();
    REQUIRE(hooksPtr->requestRefresh);
    hooksPtr->requestRefresh();

    std::size_t lateDeviceCalls = 0U;
    std::size_t lateGraphCalls = 0U;
    CHECK_FALSE(monitor.subscribeDevices([&](std::vector<Device> const&) { ++lateDeviceCalls; }));
    CHECK_FALSE(monitor.subscribeGraph("42", [&](flow::Graph const&) { ++lateGraphCalls; }));
    CHECK(monitor.enumerateSinks().empty());
    CHECK(deviceCalls.load(std::memory_order_relaxed) == deviceCallsAfterStop);
    CHECK(graphCalls.load(std::memory_order_relaxed) == graphCallsAfterStop);
    CHECK(lateDeviceCalls == 0U);
    CHECK(lateGraphCalls == 0U);
  }

  TEST_CASE("PipeWireMonitor - graph subscription racing with stop is rejected",
            "[audio][regression][pipewire][concurrency]")
  {
    auto hooksPtr = makeMonitorHooks();
    auto initialRefreshCompleted = std::binary_semaphore{0};
    auto subscriptionInserted = std::binary_semaphore{0};
    auto continueSubscription = std::binary_semaphore{0};
    auto shutdownStarted = std::binary_semaphore{0};
    hooksPtr->onRefreshComplete = [&] { initialRefreshCompleted.release(); };
    hooksPtr->onGraphSubscriptionInserted = [&]
    {
      subscriptionInserted.release();
      continueSubscription.acquire();
    };
    hooksPtr->onShutdownStarted = [&] { shutdownStarted.release(); };
    auto monitor = PipeWireMonitor{hooksPtr};
    monitor.start();
    REQUIRE(initialRefreshCompleted.try_acquire_for(kWaitTimeout));
    auto graphCalls = std::atomic{std::size_t{0U}};
    auto graphSub = Subscription{};

    auto subscriptionThread =
      std::jthread{[&]
                   {
                     graphSub = monitor.subscribeGraph(
                       "42", [&](flow::Graph const&) { graphCalls.fetch_add(1U, std::memory_order_relaxed); });
                   }};
    auto const inserted = subscriptionInserted.try_acquire_for(kWaitTimeout);

    if (!inserted)
    {
      continueSubscription.release();
      subscriptionThread.join();
    }

    REQUIRE(inserted);
    auto shutdownThread = std::jthread{[&] { monitor.stop(); }};
    auto const startedShutdown = shutdownStarted.try_acquire_for(kWaitTimeout);
    continueSubscription.release();
    subscriptionThread.join();
    shutdownThread.join();
    REQUIRE(startedShutdown);

    CHECK_FALSE(graphSub);
    CHECK(graphCalls.load(std::memory_order_relaxed) == 0U);
  }

  TEST_CASE("PipeWireMonitor - device and graph subscriptions may outlive monitor",
            "[audio][regression][pipewire][monitor]")
  {
    auto hooksPtr = makeMonitorHooks();
    auto stateDestroyed = std::binary_semaphore{0};
    hooksPtr->onMonitorStateDestroyed = [&] { stateDestroyed.release(); };
    auto deviceSub = Subscription{};
    auto graphSub = Subscription{};

    {
      auto monitor = PipeWireMonitor{hooksPtr};
      deviceSub = monitor.subscribeDevices([](std::vector<Device> const&) {});
      graphSub = monitor.subscribeGraph("42", [](flow::Graph const&) {});
      REQUIRE(deviceSub);
      REQUIRE(graphSub);
    }

    REQUIRE(stateDestroyed.try_acquire_for(kWaitTimeout));
    deviceSub.reset();
    deviceSub.reset();
    graphSub.reset();
    graphSub.reset();
    CHECK_FALSE(deviceSub);
    CHECK_FALSE(graphSub);
  }

  TEST_CASE("PipeWireProvider - status and subscriptions expose the same default-route inventory",
            "[audio][regression][pipewire][provider]")
  {
    auto provider = PipeWireProvider{makeMonitorHooks()};
    auto const status = provider.status();
    auto callbackDevices = std::vector<Device>{};

    CHECK_FALSE(provider.subscribeDevices({}));
    auto sub = provider.subscribeDevices([&](std::vector<Device> const& devices) { callbackDevices = devices; });
    REQUIRE(sub);
    provider.shutdown();

    CHECK(provider.status().devices.empty());
    REQUIRE(status.devices.size() == 2U);
    CHECK(status.devices[0].id.empty());
    CHECK(status.devices[0].isDefault);
    CHECK(status.devices[0].backendId == kBackendPipeWire);
    CHECK(status.devices[1] == syntheticSink());
    CHECK(callbackDevices == status.devices);
  }

  TEST_CASE("PipeWireProvider - initial device callback may destroy provider",
            "[audio][regression][pipewire][concurrency]")
  {
    auto hooksPtr = makeMonitorHooks();
    auto monitorExited = std::binary_semaphore{0};
    auto stateDestroyed = std::binary_semaphore{0};
    hooksPtr->onMonitorExit = [&] { monitorExited.release(); };
    hooksPtr->onMonitorStateDestroyed = [&] { stateDestroyed.release(); };
    auto providerPtr = std::make_unique<PipeWireProvider>(hooksPtr);
    auto callbackDevices = std::vector<Device>{};

    auto sub = providerPtr->subscribeDevices(
      [&](std::vector<Device> const& devices)
      {
        callbackDevices = devices;
        providerPtr.reset();
      });

    CHECK_FALSE(providerPtr);
    CHECK_FALSE(sub);
    REQUIRE(callbackDevices.size() == 2U);
    CHECK(callbackDevices[0].id.empty());
    CHECK(callbackDevices[0].isDefault);
    CHECK(callbackDevices[1].id == "91");
    REQUIRE(monitorExited.try_acquire_for(kWaitTimeout));
    REQUIRE(stateDestroyed.try_acquire_for(kWaitTimeout));
    sub.reset();
  }

  TEST_CASE("PipeWireProvider - nested provider callback may destroy the outer provider",
            "[audio][regression][pipewire][concurrency]")
  {
    auto outerRefreshCompleted = std::binary_semaphore{0};
    auto innerRefreshCompleted = std::binary_semaphore{0};
    auto outerMonitorExited = std::binary_semaphore{0};
    auto outerStateDestroyed = std::binary_semaphore{0};
    auto outerHooksPtr = makeMonitorHooks();
    auto innerHooksPtr = makeMonitorHooks();
    outerHooksPtr->onRefreshComplete = [&] { outerRefreshCompleted.release(); };
    outerHooksPtr->onMonitorExit = [&] { outerMonitorExited.release(); };
    outerHooksPtr->onMonitorStateDestroyed = [&] { outerStateDestroyed.release(); };
    innerHooksPtr->onRefreshComplete = [&] { innerRefreshCompleted.release(); };
    auto outerProviderPtr = std::make_unique<PipeWireProvider>(outerHooksPtr);
    auto innerProvider = PipeWireProvider{innerHooksPtr};
    REQUIRE(outerRefreshCompleted.try_acquire_for(kWaitTimeout));
    REQUIRE(innerRefreshCompleted.try_acquire_for(kWaitTimeout));
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

  TEST_CASE("PipeWireProvider - graph worker callback may destroy provider",
            "[audio][regression][pipewire][concurrency]")
  {
    auto hooksPtr = makeMonitorHooks();
    auto monitorExited = std::binary_semaphore{0};
    auto stateDestroyed = std::binary_semaphore{0};
    auto providerReleased = std::binary_semaphore{0};
    auto releaseCallback = std::binary_semaphore{0};
    hooksPtr->onMonitorExit = [&] { monitorExited.release(); };
    hooksPtr->onMonitorStateDestroyed = [&] { stateDestroyed.release(); };
    auto providerPtr = std::make_unique<PipeWireProvider>(hooksPtr);
    auto callbackCount = std::atomic{std::size_t{0U}};
    auto callbackNodeId = std::string{};

    auto graphSub = providerPtr->subscribeGraph("42",
                                                [&](flow::Graph const& graph)
                                                {
                                                  callbackCount.fetch_add(1U, std::memory_order_relaxed);

                                                  if (!graph.nodes.empty())
                                                  {
                                                    callbackNodeId = graph.nodes.front().id;
                                                  }

                                                  providerPtr.reset();
                                                  providerReleased.release();
                                                  releaseCallback.acquire();
                                                });

    REQUIRE(providerReleased.try_acquire_for(kWaitTimeout));
    CHECK_FALSE(providerPtr);
    CHECK_FALSE(stateDestroyed.try_acquire());
    releaseCallback.release();
    REQUIRE(monitorExited.try_acquire_for(kWaitTimeout));
    REQUIRE(stateDestroyed.try_acquire_for(kWaitTimeout));
    CHECK(callbackCount.load(std::memory_order_relaxed) == 1U);
    CHECK(callbackNodeId == "42");
    graphSub.reset();
    CHECK_FALSE(graphSub);
  }

  TEST_CASE("PipeWireProvider - device and graph subscriptions may outlive provider",
            "[audio][regression][pipewire][provider]")
  {
    auto deviceSub = Subscription{};
    auto graphSub = Subscription{};

    {
      auto provider = PipeWireProvider{makeMonitorHooks()};
      deviceSub = provider.subscribeDevices([](std::vector<Device> const&) {});
      graphSub = provider.subscribeGraph("42", [](flow::Graph const&) {});
      REQUIRE(deviceSub);
      REQUIRE(graphSub);
    }

    deviceSub.reset();
    graphSub.reset();
    CHECK_FALSE(deviceSub);
    CHECK_FALSE(graphSub);
  }

  TEST_CASE("PipeWireMonitor - concurrent subscriptions retain distinct registrations",
            "[audio][regression][pipewire][concurrency]")
  {
    auto hooksPtr = makeMonitorHooks();
    auto monitor = PipeWireMonitor{hooksPtr};
    auto secondRefreshed = std::binary_semaphore{0};
    auto firstCalls = std::atomic{std::size_t{0U}};
    auto secondCalls = std::atomic{std::size_t{0U}};
    auto firstSub = Subscription{};
    auto secondSub = Subscription{};
    auto firstThread =
      std::jthread{[&]
                   {
                     firstSub = monitor.subscribeDevices([&](std::vector<Device> const&)
                                                         { firstCalls.fetch_add(1U, std::memory_order_relaxed); });
                   }};
    auto secondThread = std::jthread{[&]
                                     {
                                       secondSub = monitor.subscribeDevices(
                                         [&](std::vector<Device> const&)
                                         {
                                           if (secondCalls.fetch_add(1U, std::memory_order_relaxed) == 1U)
                                           {
                                             secondRefreshed.release();
                                           }
                                         });
                                     }};
    firstThread.join();
    secondThread.join();
    REQUIRE(firstSub);
    REQUIRE(secondSub);
    CHECK(firstCalls.load(std::memory_order_relaxed) == 1U);
    CHECK(secondCalls.load(std::memory_order_relaxed) == 1U);
    firstSub.reset();
    REQUIRE(hooksPtr->requestRefresh);

    hooksPtr->requestRefresh();

    REQUIRE(secondRefreshed.try_acquire_for(kWaitTimeout));
    CHECK(firstCalls.load(std::memory_order_relaxed) == 1U);
    CHECK(secondCalls.load(std::memory_order_relaxed) == 2U);
    monitor.stop();
  }

  TEST_CASE("PipeWireMonitor - reset suppresses a callback copied by refresh",
            "[audio][regression][pipewire][concurrency]")
  {
    auto hooksPtr = makeMonitorHooks();
    auto refreshPrepared = std::binary_semaphore{0};
    auto continueRefresh = std::binary_semaphore{0};
    auto firstDeliveredAfterReset = std::binary_semaphore{0};
    auto blockPreparedRefresh = std::atomic_bool{false};
    auto observeAfterReset = std::atomic_bool{false};
    hooksPtr->onRefreshPrepared = [&]
    {
      if (blockPreparedRefresh.exchange(false, std::memory_order_acq_rel))
      {
        refreshPrepared.release();
        continueRefresh.acquire();
      }
    };
    auto monitor = PipeWireMonitor{hooksPtr};
    auto firstCalls = std::atomic{std::size_t{0U}};
    auto secondCalls = std::atomic{std::size_t{0U}};
    auto secondInitiallyDelivered = std::binary_semaphore{0};
    auto firstSub = monitor.subscribeGraph("42",
                                           [&](flow::Graph const&)
                                           {
                                             firstCalls.fetch_add(1U, std::memory_order_relaxed);

                                             if (observeAfterReset.load(std::memory_order_acquire))
                                             {
                                               firstDeliveredAfterReset.release();
                                             }
                                           });
    auto secondSub = monitor.subscribeGraph("43",
                                            [&](flow::Graph const&)
                                            {
                                              if (secondCalls.fetch_add(1U, std::memory_order_relaxed) == 0U)
                                              {
                                                secondInitiallyDelivered.release();
                                              }
                                            });
    REQUIRE(firstSub);
    REQUIRE(secondSub);
    REQUIRE(secondInitiallyDelivered.try_acquire_for(kWaitTimeout));
    blockPreparedRefresh.store(true, std::memory_order_release);
    REQUIRE(hooksPtr->requestRefresh);
    hooksPtr->requestRefresh();
    REQUIRE(refreshPrepared.try_acquire_for(kWaitTimeout));
    auto const secondCallsBeforeReset = secondCalls.load(std::memory_order_relaxed);

    secondSub.reset();
    observeAfterReset.store(true, std::memory_order_release);
    continueRefresh.release();

    REQUIRE(firstDeliveredAfterReset.try_acquire_for(kWaitTimeout));
    CHECK(secondCalls.load(std::memory_order_relaxed) == secondCallsBeforeReset);
    monitor.stop();
  }

  TEST_CASE("PipeWireMonitor - concurrent external shutdown callers share callback quiescence",
            "[audio][regression][pipewire][concurrency]")
  {
    auto hooksPtr = makeMonitorHooks();
    auto callbackEntered = std::binary_semaphore{0};
    auto releaseCallback = std::binary_semaphore{0};
    auto shutdownStarted = std::binary_semaphore{0};
    auto shutdownWait = std::binary_semaphore{0};
    auto firstReturned = std::binary_semaphore{0};
    auto secondReturned = std::binary_semaphore{0};
    hooksPtr->onShutdownStarted = [&] { shutdownStarted.release(); };
    hooksPtr->onShutdownWait = [&] { shutdownWait.release(); };
    auto monitor = PipeWireMonitor{hooksPtr};
    auto blockCallback = std::atomic_bool{false};
    auto sub = monitor.subscribeDevices(
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
                                        monitor.stop();
                                        firstReturned.release();
                                      }};
    REQUIRE(shutdownStarted.try_acquire_for(kWaitTimeout));
    auto secondShutdown = std::jthread{[&]
                                       {
                                         monitor.stop();
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
  }

  TEST_CASE("PipeWireMonitor - callback shutdown returns while later external shutdown waits",
            "[audio][regression][pipewire][concurrency]")
  {
    auto hooksPtr = makeMonitorHooks();
    auto callbackShutdownReturned = std::binary_semaphore{0};
    auto releaseCallback = std::binary_semaphore{0};
    auto shutdownWait = std::binary_semaphore{0};
    auto externalReturned = std::binary_semaphore{0};
    auto monitorExited = std::binary_semaphore{0};
    hooksPtr->onShutdownWait = [&] { shutdownWait.release(); };
    hooksPtr->onMonitorExit = [&] { monitorExited.release(); };
    auto monitor = PipeWireMonitor{hooksPtr};
    auto callbackCount = std::atomic{std::size_t{0U}};
    auto lateCalls = std::atomic{std::size_t{0U}};
    auto lateSub = Subscription{};
    auto sub = monitor.subscribeDevices(
      [&](std::vector<Device> const&)
      {
        if (callbackCount.fetch_add(1U, std::memory_order_relaxed) == 0U)
        {
          return;
        }

        monitor.stop();
        lateSub = monitor.subscribeDevices([&](std::vector<Device> const&)
                                           { lateCalls.fetch_add(1U, std::memory_order_relaxed); });
        callbackShutdownReturned.release();
        releaseCallback.acquire();
      });
    REQUIRE(sub);
    REQUIRE(hooksPtr->requestRefresh);

    hooksPtr->requestRefresh();
    REQUIRE(callbackShutdownReturned.try_acquire_for(kWaitTimeout));
    auto externalShutdown = std::jthread{[&]
                                         {
                                           monitor.stop();
                                           externalReturned.release();
                                         }};
    REQUIRE(shutdownWait.try_acquire_for(kWaitTimeout));

    CHECK_FALSE(externalReturned.try_acquire());
    CHECK_FALSE(lateSub);
    CHECK(lateCalls.load(std::memory_order_relaxed) == 0U);
    releaseCallback.release();
    externalShutdown.join();
    REQUIRE(monitorExited.try_acquire_for(kWaitTimeout));
    CHECK(externalReturned.try_acquire());
    CHECK(callbackCount.load(std::memory_order_relaxed) == 2U);
    CHECK_FALSE(monitor.subscribeDevices([](std::vector<Device> const&) {}));
  }
} // namespace ao::audio::backend::test
