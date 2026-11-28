// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/backend/PipeWireMonitor.h"

#include "lib/audio/backend/PipeWireProvider.h"
#include "lib/audio/backend/detail/PipeWireMonitorHooks.h"
#include "test/runtime/AsyncTestSupport.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/flow/Graph.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <semaphore>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ao::audio::backend::test
{
  namespace
  {
    constexpr auto kWaitTimeout = std::chrono::seconds{5};

    struct WorkerPause final
    {
      void park()
      {
        entered.release();
        resume.acquire();
      }

      std::binary_semaphore entered{0};
      std::binary_semaphore resume{0};
    };

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

  TEST_CASE("PipeWireMonitor - stop closes publication and later subscription admission", "[concurrency]")
  {
    auto hooksPtr = makeMonitorHooks();
    auto monitor = PipeWireMonitor{hooksPtr};
    auto graphDelivered = std::binary_semaphore{0};
    auto deviceCalls = std::atomic<std::size_t>{0U};
    auto graphCalls = std::atomic<std::size_t>{0U};
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

  TEST_CASE("PipeWireMonitor - refresh repeats unchanged snapshots without an inline initial graph", "[concurrency]")
  {
    auto refreshCompleted = std::counting_semaphore{0};
    auto deviceSnapshots = std::vector<std::vector<Device>>{};
    auto graphs = std::vector<flow::Graph>{};
    auto deviceDeliveredInline = std::atomic_bool{false};
    auto graphDeliveredInline = std::atomic_bool{false};
    auto const subscriberThread = std::this_thread::get_id();
    auto hooksPtr = makeMonitorHooks();
    hooksPtr->onRefreshComplete = [&] { refreshCompleted.release(); };
    auto monitor = PipeWireMonitor{hooksPtr};
    auto deviceSub = monitor.subscribeDevices(
      [&](std::vector<Device> const& devices)
      {
        if (std::this_thread::get_id() == subscriberThread)
        {
          deviceDeliveredInline.store(true);
        }

        deviceSnapshots.push_back(devices);
      });
    REQUIRE(deviceSub);
    CHECK(deviceDeliveredInline.load());

    auto graphSub = monitor.subscribeGraph("42",
                                           [&](flow::Graph const& graph)
                                           {
                                             if (std::this_thread::get_id() == subscriberThread)
                                             {
                                               graphDeliveredInline.store(true);
                                             }

                                             graphs.push_back(graph);
                                           });
    REQUIRE(graphSub);
    REQUIRE(refreshCompleted.try_acquire_for(kWaitTimeout));
    REQUIRE(hooksPtr->requestRefresh);
    hooksPtr->requestRefresh();
    REQUIRE(refreshCompleted.try_acquire_for(kWaitTimeout));
    monitor.stop();

    CHECK_FALSE(graphDeliveredInline.load());
    auto const sinks = std::vector{syntheticSink()};
    CHECK((deviceSnapshots == std::vector{sinks, sinks, sinks}));
    REQUIRE(graphs.size() == 2U);
    REQUIRE(graphs.front().nodes.size() == 1U);
    CHECK(graphs.front().nodes.front().id == "42");
    CHECK(graphs.back() == graphs.front());
  }

  TEST_CASE("PipeWireMonitor - graph subscription racing with stop is rejected", "[concurrency]")
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
    auto graphCalls = std::atomic<std::size_t>{0U};
    auto graphSub = utility::ScopedRegistration{};

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

  TEST_CASE("PipeWireMonitor - device and graph subscriptions may outlive monitor", "[concurrency]")
  {
    auto hooksPtr = makeMonitorHooks();
    auto stateDestroyed = std::binary_semaphore{0};
    hooksPtr->onMonitorStateDestroyed = [&] { stateDestroyed.release(); };
    auto deviceSub = utility::ScopedRegistration{};
    auto graphSub = utility::ScopedRegistration{};

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

  TEST_CASE("PipeWireProvider - status and subscriptions expose the same default-route inventory")
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

  TEST_CASE("PipeWireProvider - initial device callback may destroy provider", "[concurrency]")
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

  TEST_CASE("PipeWireProvider - nested provider callback may destroy the outer provider", "[concurrency]")
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
    auto innerSub = utility::ScopedRegistration{};
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

  TEST_CASE("PipeWireProvider - graph worker callback may destroy provider", "[concurrency]")
  {
    auto hooksPtr = makeMonitorHooks();
    auto monitorExited = std::binary_semaphore{0};
    auto stateDestroyed = std::binary_semaphore{0};
    auto providerReleased = std::binary_semaphore{0};
    auto releaseCallback = std::binary_semaphore{0};
    hooksPtr->onMonitorExit = [&] { monitorExited.release(); };
    hooksPtr->onMonitorStateDestroyed = [&] { stateDestroyed.release(); };
    auto providerPtr = std::make_unique<PipeWireProvider>(hooksPtr);
    auto callbackCount = std::atomic<std::size_t>{0U};
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

  TEST_CASE("PipeWireProvider - device and graph subscriptions may outlive provider", "[concurrency]")
  {
    auto deviceSub = utility::ScopedRegistration{};
    auto graphSub = utility::ScopedRegistration{};

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

  TEST_CASE("PipeWireMonitor - concurrent subscriptions retain distinct registrations", "[concurrency]")
  {
    auto hooksPtr = makeMonitorHooks();
    auto monitor = PipeWireMonitor{hooksPtr};
    auto secondRefreshed = std::binary_semaphore{0};
    auto firstCalls = std::atomic<std::size_t>{0U};
    auto secondCalls = std::atomic<std::size_t>{0U};
    auto firstSub = utility::ScopedRegistration{};
    auto secondSub = utility::ScopedRegistration{};
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

  TEST_CASE("PipeWireMonitor - external reset releases a batched capture before its turn", "[concurrency]")
  {
    auto processDeadline = rt::test::ScopedTestDeadline{};
    auto earlierCallback = WorkerPause{};
    auto refreshCompleted = std::counting_semaphore{0};
    auto resetReturned = std::binary_semaphore{0};
    auto parkNextUpdate = std::atomic_bool{false};
    std::size_t laterCalls = 0U;
    auto survivorGraphs = std::vector<flow::Graph>{};
    auto const refreshCaller = std::this_thread::get_id();
    auto hooksPtr = makeMonitorHooks();
    hooksPtr->onRefreshComplete = [&] { refreshCompleted.release(); };
    auto monitor = PipeWireMonitor{hooksPtr};
    auto earlierSub = monitor.subscribeGraph("42",
                                             [&](flow::Graph const& graph)
                                             {
                                               if (parkNextUpdate.exchange(false))
                                               {
                                                 earlierCallback.park();
                                               }

                                               if (std::this_thread::get_id() == refreshCaller)
                                               {
                                                 survivorGraphs.push_back(graph);
                                               }
                                             });
    REQUIRE(earlierSub);
    REQUIRE(refreshCompleted.try_acquire_for(kWaitTimeout));
    auto capturePtr = std::make_shared<int>(1);
    auto const weakCapturePtr = std::weak_ptr<int>{capturePtr};
    auto const makeLaterCallback = [&]
    { return [&, capturePtr = std::move(capturePtr)](auto const&) { ++laterCalls; }; };
    auto laterSub = utility::ScopedRegistration{};

    SECTION("later graph callback on the same route")
    {
      laterSub = monitor.subscribeGraph("42", makeLaterCallback());
      REQUIRE(refreshCompleted.try_acquire_for(kWaitTimeout));
    }

    SECTION("later graph callback on a different route")
    {
      laterSub = monitor.subscribeGraph("43", makeLaterCallback());
      REQUIRE(refreshCompleted.try_acquire_for(kWaitTimeout));
    }

    SECTION("later device callback")
    {
      laterSub = monitor.subscribeDevices(makeLaterCallback());
    }

    REQUIRE(laterSub);
    REQUIRE(laterCalls == 1U);
    REQUIRE_FALSE(weakCapturePtr.expired());
    REQUIRE(hooksPtr->requestRefresh);
    auto resetThread = std::jthread{};
    auto resumeOnExit = utility::ScopedRegistration{[&] { earlierCallback.resume.release(); }};
    // Pause within publication, when the later recipient is already batched but has not run.
    parkNextUpdate.store(true);
    hooksPtr->requestRefresh();
    REQUIRE(earlierCallback.entered.try_acquire_for(kWaitTimeout));

    resetThread = std::jthread{[&]
                               {
                                 laterSub.reset();
                                 resetReturned.release();
                               }};
    bool const resetWhileParked = resetReturned.try_acquire_for(kWaitTimeout);
    bool const captureReleasedWhileParked = weakCapturePtr.expired();
    resumeOnExit.reset();
    resetThread.join();
    REQUIRE(refreshCompleted.try_acquire_for(kWaitTimeout));

    // Observe this explicit refresh independently of any worker refresh requested by reset.
    monitor.refresh();
    REQUIRE(survivorGraphs.size() == 1U);
    REQUIRE(survivorGraphs.front().nodes.size() == 1U);
    CHECK(survivorGraphs.front().nodes.front().id == "42");
    monitor.stop();

    CHECK(resetWhileParked);
    CHECK(captureReleasedWhileParked);
    CHECK(laterCalls == 1U);
  }

  TEST_CASE("PipeWireMonitor - reset keeps an active device capture alive until callback exit", "[concurrency]")
  {
    auto processDeadline = rt::test::ScopedTestDeadline{};
    auto activeCallback = WorkerPause{};
    auto refreshCompleted = std::counting_semaphore{0};
    auto resetStarted = std::binary_semaphore{0};
    auto resetReturned = std::binary_semaphore{0};
    auto leavingCallback = std::atomic_bool{false};
    std::size_t calls = 0U;
    bool resetFromCallback = false;
    bool resetObservedExit = false;

    SECTION("external reset")
    {
    }

    SECTION("self reset")
    {
      resetFromCallback = true;
    }

    auto hooksPtr = makeMonitorHooks();
    hooksPtr->onRefreshComplete = [&] { refreshCompleted.release(); };
    auto monitor = PipeWireMonitor{hooksPtr};
    auto capturePtr = std::make_shared<int>(1);
    auto const weakCapturePtr = std::weak_ptr<int>{capturePtr};
    auto sub = utility::ScopedRegistration{};
    sub = monitor.subscribeDevices(
      [&, capturePtr = std::move(capturePtr)](std::vector<Device> const&)
      {
        if (++calls != 2U)
        {
          return;
        }

        if (resetFromCallback)
        {
          sub.reset();
        }

        activeCallback.park();
        leavingCallback.store(true);
      });
    REQUIRE(sub);
    REQUIRE(hooksPtr->requestRefresh);
    auto resetThread = std::jthread{};
    auto resumeOnExit = utility::ScopedRegistration{[&] { activeCallback.resume.release(); }};
    hooksPtr->requestRefresh();
    REQUIRE(activeCallback.entered.try_acquire_for(kWaitTimeout));

    if (resetFromCallback)
    {
      CHECK_FALSE(sub);
    }
    else
    {
      resetThread = std::jthread{[&]
                                 {
                                   resetStarted.release();
                                   sub.reset();
                                   resetObservedExit = leavingCallback.load();
                                   resetReturned.release();
                                 }};
      REQUIRE(resetStarted.try_acquire_for(kWaitTimeout));
      // Also check the observed exit order after joining; the window only exercises early return.
      CHECK_FALSE(resetReturned.try_acquire_for(std::chrono::milliseconds{100}));
    }

    CHECK_FALSE(weakCapturePtr.expired());
    resumeOnExit.reset();

    if (resetThread.joinable())
    {
      resetThread.join();
      CHECK(resetObservedExit);
    }

    REQUIRE(refreshCompleted.try_acquire_for(kWaitTimeout));
    CHECK(weakCapturePtr.expired());
    hooksPtr->requestRefresh();
    REQUIRE(refreshCompleted.try_acquire_for(kWaitTimeout));
    monitor.stop();
    CHECK(calls == 2U);
  }

  TEST_CASE("PipeWireMonitor - retiring a subscription capture may reenter stop", "[concurrency]")
  {
    auto processDeadline = rt::test::ScopedTestDeadline{};
    auto refreshCompleted = std::counting_semaphore{0};
    auto capturesRetired = std::atomic<std::size_t>{0U};
    auto hooksPtr = makeMonitorHooks();
    hooksPtr->onRefreshComplete = [&] { refreshCompleted.release(); };
    auto monitor = PipeWireMonitor{hooksPtr};
    auto const makeCapture = [&]
    {
      return std::make_shared<utility::ScopedRegistration>(
        [&]
        {
          monitor.stop();
          capturesRetired.fetch_add(1U);
        });
    };
    auto deviceSub = monitor.subscribeDevices([capturePtr = makeCapture()](std::vector<Device> const&) {});
    auto graphSub = monitor.subscribeGraph("42", [capturePtr = makeCapture()](flow::Graph const&) {});
    REQUIRE(deviceSub);
    REQUIRE(graphSub);
    REQUIRE(refreshCompleted.try_acquire_for(kWaitTimeout));
    REQUIRE(capturesRetired.load() == 0U);

    SECTION("stop retires the captures")
    {
      monitor.stop();
    }

    SECTION("reset retires the graph capture and initiates stop")
    {
      graphSub.reset();
    }

    CHECK(capturesRetired.load() == 2U);
    CHECK(monitor.enumerateSinks().empty());
    CHECK_FALSE(monitor.subscribeGraph("42", [](flow::Graph const&) {}));
  }

  TEST_CASE("PipeWireMonitor - concurrent external shutdown callers share callback quiescence", "[concurrency]")
  {
    auto hooksPtr = makeMonitorHooks();
    auto callbackEntered = std::binary_semaphore{0};
    auto releaseCallback = std::binary_semaphore{0};
    auto shutdownStarted = std::binary_semaphore{0};
    // Allow the external wait plus monitor and worker teardown notifications.
    auto shutdownWait = std::counting_semaphore<3>{0};
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

  TEST_CASE("PipeWireMonitor - external stop drains a self-reset callback on the public refresh caller",
            "[concurrency]")
  {
    auto processDeadline = rt::test::ScopedTestDeadline{};
    auto callback = WorkerPause{};
    auto destroyingCapture = WorkerPause{};
    auto workerPrepared = std::binary_semaphore{0};
    auto monitorExited = std::binary_semaphore{0};
    auto expectWorkerRefresh = std::atomic_bool{false};
    auto callbackLeaving = std::atomic_bool{false};
    auto captureFinished = std::atomic_bool{false};
    auto stopReturned = std::atomic_bool{false};
    bool stopObservedQuiescence = false;
    std::size_t calls = 0U;
    std::size_t laterCalls = 0U;
    auto hooksPtr = makeMonitorHooks();
    hooksPtr->onRefreshPrepared = [&]
    {
      if (expectWorkerRefresh.exchange(false))
      {
        workerPrepared.release();
      }
    };
    hooksPtr->onMonitorExit = [&] { monitorExited.release(); };
    auto monitor = PipeWireMonitor{hooksPtr};
    auto sub = utility::ScopedRegistration{};
    auto laterSub = utility::ScopedRegistration{};
    auto refreshCaller = std::jthread{};
    auto externalShutdown = std::jthread{};
    // Release both pauses before joining either thread or retiring the subscriptions on failure.
    auto resumeCapture = utility::ScopedRegistration{[&] { destroyingCapture.resume.release(); }};
    auto resumeCallback = utility::ScopedRegistration{[&] { callback.resume.release(); }};
    auto capturePtr = std::make_shared<utility::ScopedRegistration>(
      [&]
      {
        if (callbackLeaving.load())
        {
          destroyingCapture.park();
        }

        captureFinished.store(true);
      });
    auto const weakCapturePtr = std::weak_ptr{capturePtr};
    sub = monitor.subscribeDevices(
      [&, capturePtr = std::move(capturePtr)](std::vector<Device> const&)
      {
        if (++calls == 2U)
        {
          sub.reset();
          callback.park();
          callbackLeaving.store(true);
        }
      });
    laterSub = monitor.subscribeDevices([&](std::vector<Device> const&) { ++laterCalls; });
    REQUIRE(sub);
    REQUIRE(laterSub);
    REQUIRE(hooksPtr->requestRefresh);

    refreshCaller = std::jthread{[&] { monitor.refresh(); }};
    REQUIRE(callback.entered.try_acquire_for(kWaitTimeout));
    CHECK_FALSE(sub);
    // Self-reset removed the active subscription; the later subscriber still gives the worker pending work.
    expectWorkerRefresh.store(true);
    hooksPtr->requestRefresh();
    REQUIRE(workerPrepared.try_acquire_for(kWaitTimeout));

    externalShutdown = std::jthread{[&]
                                    {
                                      monitor.stop();
                                      stopObservedQuiescence = callbackLeaving.load() && captureFinished.load();
                                      stopReturned.store(true);
                                    }};
    // The refresh worker can exit even though a callback is still running on the public caller.
    REQUIRE(monitorExited.try_acquire_for(kWaitTimeout));
    CHECK_FALSE(stopReturned.load());
    CHECK_FALSE(weakCapturePtr.expired());

    resumeCallback.reset();
    REQUIRE(destroyingCapture.entered.try_acquire_for(kWaitTimeout));
    CHECK_FALSE(stopReturned.load());
    resumeCapture.reset();
    refreshCaller.join();
    externalShutdown.join();

    CHECK(stopReturned.load());
    CHECK(stopObservedQuiescence);
    CHECK(weakCapturePtr.expired());
    monitor.refresh();
    CHECK(calls == 2U);
    CHECK(laterCalls == 1U); // Neither refresh may deliver its pending update to the later subscriber.
  }

  TEST_CASE("PipeWireMonitor - monitor callback stop returns before external shutdown quiesces", "[concurrency]")
  {
    auto processDeadline = rt::test::ScopedTestDeadline{};
    auto stoppingCallback = WorkerPause{};
    auto exitingMonitor = WorkerPause{};
    auto shutdownWait = std::counting_semaphore{0};
    auto externalReturned = std::binary_semaphore{0};
    auto monitorReleased = std::atomic_bool{false};
    std::size_t callbackCount = 0U;
    std::size_t batchedCalls = 0U;
    std::size_t lateCalls = 0U;
    bool externalObservedQuiescence = false;
    auto lateSub = utility::ScopedRegistration{};
    auto hooksPtr = makeMonitorHooks();
    hooksPtr->onShutdownWait = [&] { shutdownWait.release(); };
    auto monitor = PipeWireMonitor{hooksPtr};
    auto capturePtr = std::make_shared<int>(1);
    auto const weakCapturePtr = std::weak_ptr<int>{capturePtr};
    auto sub = utility::ScopedRegistration{};
    auto batchedSub = utility::ScopedRegistration{};
    auto externalShutdown = std::jthread{};
    // Unpark both worker boundaries before joining or resetting subscriptions on failure.
    auto resumeMonitor = utility::ScopedRegistration{[&] { exitingMonitor.resume.release(); }};
    auto resumeCallback = utility::ScopedRegistration{[&] { stoppingCallback.resume.release(); }};
    hooksPtr->onMonitorExit = [&]
    {
      // The worker can still reenter stop after the initiating user callback has unwound.
      monitor.stop();
      exitingMonitor.park();
      monitorReleased.store(true);
    };
    sub = monitor.subscribeDevices(
      [&](std::vector<Device> const&)
      {
        if (++callbackCount == 2U)
        {
          monitor.stop();
          lateSub = monitor.subscribeDevices([&](std::vector<Device> const&) { ++lateCalls; });
          stoppingCallback.park();
        }
      });
    batchedSub =
      monitor.subscribeDevices([&, capturePtr = std::move(capturePtr)](std::vector<Device> const&) { ++batchedCalls; });
    REQUIRE(sub);
    REQUIRE(batchedSub);
    REQUIRE(hooksPtr->requestRefresh);

    hooksPtr->requestRefresh();
    REQUIRE(stoppingCallback.entered.try_acquire_for(kWaitTimeout));
    externalShutdown = std::jthread{[&]
                                    {
                                      monitor.stop();
                                      externalObservedQuiescence = monitorReleased.load() && weakCapturePtr.expired();
                                      externalReturned.release();
                                    }};
    REQUIRE(shutdownWait.try_acquire_for(kWaitTimeout));
    CHECK_FALSE(externalReturned.try_acquire());
    CHECK_FALSE(lateSub);
    CHECK(lateCalls == 0U);

    resumeCallback.reset();
    REQUIRE(exitingMonitor.entered.try_acquire_for(kWaitTimeout));
    CHECK_FALSE(externalReturned.try_acquire());
    resumeMonitor.reset();
    externalShutdown.join();
    CHECK(externalReturned.try_acquire());
    CHECK(externalObservedQuiescence);
    CHECK(callbackCount == 2U);
    CHECK(batchedCalls == 1U);
    CHECK_FALSE(monitor.subscribeDevices([](std::vector<Device> const&) {}));
  }
} // namespace ao::audio::backend::test
