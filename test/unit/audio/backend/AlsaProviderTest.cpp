// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/AlsaProvider.h"

#include "lib/audio/backend/detail/AlsaProviderMonitorHooks.h"
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Property.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <semaphore>
#include <thread>
#include <vector>

namespace ao::audio::backend::test
{
  constexpr auto kWaitTimeout = std::chrono::seconds{5};

  namespace
  {
    Device syntheticDevice()
    {
      return {.id = DeviceId{"hw:127,0"},
              .displayName = "Synthetic ALSA Output",
              .description = "hw:127,0",
              .backendId = kBackendAlsa};
    }

    std::shared_ptr<detail::AlsaProviderMonitorHooks> makeMonitorHooks()
    {
      auto hooksPtr = std::make_shared<detail::AlsaProviderMonitorHooks>();
      hooksPtr->enumerateDevices = [] { return std::vector{syntheticDevice()}; };
      return hooksPtr;
    }
  } // namespace

  TEST_CASE("AlsaProvider - exposes exclusive devices and rejects subscriptions after shutdown",
            "[audio][unit][alsa][provider]")
  {
    auto hooksPtr = makeMonitorHooks();
    auto provider = AlsaProvider{hooksPtr};
    auto const status = provider.status();

    CHECK(status.descriptor.id == kBackendAlsa);
    REQUIRE(status.descriptor.supportedProfiles.size() == 1U);
    CHECK(status.descriptor.supportedProfiles.front().id == kProfileExclusive);
    REQUIRE(status.devices.size() == 1U);
    CHECK(status.devices.front().id == "hw:127,0");

    provider.shutdown();
    provider.shutdown();

    CHECK(provider.status().devices.empty());
    std::size_t deviceCalls = 0U;
    std::size_t graphCalls = 0U;
    CHECK_FALSE(provider.subscribeDevices([&](std::vector<Device> const&) { ++deviceCalls; }));
    CHECK_FALSE(provider.subscribeGraph("hw:127,0", [&](flow::Graph const&) { ++graphCalls; }));
    CHECK(deviceCalls == 0U);
    CHECK(graphCalls == 0U);
  }

  TEST_CASE("AlsaProvider - initial device callback may destroy provider", "[audio][regression][alsa][concurrency]")
  {
    auto monitorExited = std::binary_semaphore{0};
    auto monitorStateDestroyed = std::binary_semaphore{0};
    auto hooksPtr = makeMonitorHooks();
    hooksPtr->onMonitorExit = [&] { monitorExited.release(); };
    hooksPtr->onMonitorStateDestroyed = [&] { monitorStateDestroyed.release(); };
    auto providerPtr = std::make_unique<AlsaProvider>(hooksPtr);
    std::size_t callbackCount = 0U;

    auto sub = providerPtr->subscribeDevices(
      [&](std::vector<Device> const& devices)
      {
        ++callbackCount;

        if (!devices.empty())
        {
          providerPtr.reset();
        }
      });

    CHECK(callbackCount == 1U);
    CHECK_FALSE(providerPtr);
    CHECK_FALSE(sub);
    REQUIRE(monitorExited.try_acquire_for(kWaitTimeout));
    REQUIRE(monitorStateDestroyed.try_acquire_for(kWaitTimeout));
    sub.reset();
  }

  TEST_CASE("AlsaProvider - nested provider callback may destroy the outer provider",
            "[audio][regression][alsa][concurrency]")
  {
    auto outerMonitorExited = std::binary_semaphore{0};
    auto outerStateDestroyed = std::binary_semaphore{0};
    auto outerHooksPtr = makeMonitorHooks();
    outerHooksPtr->onMonitorExit = [&] { outerMonitorExited.release(); };
    outerHooksPtr->onMonitorStateDestroyed = [&] { outerStateDestroyed.release(); };
    auto outerProviderPtr = std::make_unique<AlsaProvider>(outerHooksPtr);
    auto innerProvider = AlsaProvider{makeMonitorHooks()};
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

  TEST_CASE("AlsaProvider - monitor callback may destroy provider on its worker",
            "[audio][regression][alsa][concurrency]")
  {
    auto monitorExited = std::binary_semaphore{0};
    auto monitorStateDestroyed = std::binary_semaphore{0};
    auto hooksPtr = makeMonitorHooks();
    hooksPtr->onMonitorExit = [&] { monitorExited.release(); };
    hooksPtr->onMonitorStateDestroyed = [&] { monitorStateDestroyed.release(); };
    auto providerPtr = std::make_unique<AlsaProvider>(hooksPtr);
    auto callbackCount = std::atomic{std::size_t{0U}};
    auto sub = providerPtr->subscribeDevices(
      [&](std::vector<Device> const&)
      {
        if (callbackCount.fetch_add(1U, std::memory_order_relaxed) != 0U)
        {
          providerPtr.reset();
        }
      });
    REQUIRE(sub);
    REQUIRE(hooksPtr->requestRefresh);

    hooksPtr->requestRefresh();

    REQUIRE(monitorExited.try_acquire_for(kWaitTimeout));
    REQUIRE(monitorStateDestroyed.try_acquire_for(kWaitTimeout));
    CHECK(callbackCount.load(std::memory_order_relaxed) == 2U);
    CHECK_FALSE(providerPtr);
    sub.reset();
    CHECK_FALSE(sub);
  }

  TEST_CASE("AlsaProvider - graph callback may destroy provider and retained backend stays inert",
            "[audio][regression][alsa][concurrency]")
  {
    auto monitorExited = std::binary_semaphore{0};
    auto hooksPtr = makeMonitorHooks();
    hooksPtr->onMonitorExit = [&] { monitorExited.release(); };
    auto providerPtr = std::make_unique<AlsaProvider>(hooksPtr);
    std::size_t graphUpdateCount = 0U;
    bool destroyProvider = false;
    auto graphSub = providerPtr->subscribeGraph("hw:127,0",
                                                [&](flow::Graph const&)
                                                {
                                                  ++graphUpdateCount;

                                                  if (destroyProvider)
                                                  {
                                                    providerPtr.reset();
                                                  }
                                                });
    auto backendPtr = providerPtr->createBackend(syntheticDevice(), kProfileExclusive);
    REQUIRE(graphSub);
    REQUIRE(backendPtr);
    destroyProvider = true;

    REQUIRE(backendPtr->set(props::kVolume, 0.5F));

    CHECK_FALSE(providerPtr);
    REQUIRE(monitorExited.try_acquire_for(kWaitTimeout));
    REQUIRE(graphUpdateCount == 2U);
    REQUIRE(backendPtr->set(props::kVolume, 0.25F));
    backendPtr->close();
    CHECK(graphUpdateCount == 2U);
    graphSub.reset();
    CHECK_FALSE(graphSub);
  }

  TEST_CASE("AlsaProvider - device and graph subscriptions may outlive provider", "[audio][regression][alsa][provider]")
  {
    auto deviceSub = Subscription{};
    auto graphSub = Subscription{};

    {
      auto provider = AlsaProvider{makeMonitorHooks()};
      deviceSub = provider.subscribeDevices([](std::vector<Device> const&) {});
      graphSub = provider.subscribeGraph("hw:127,0", [](flow::Graph const&) {});
      REQUIRE(deviceSub);
      REQUIRE(graphSub);
    }

    deviceSub.reset();
    deviceSub.reset();
    graphSub.reset();
    graphSub.reset();
    CHECK_FALSE(deviceSub);
    CHECK_FALSE(graphSub);
  }

  TEST_CASE("AlsaProvider - concurrent subscriptions retain distinct registrations",
            "[audio][regression][alsa][concurrency]")
  {
    auto refreshComplete = std::binary_semaphore{0};
    auto hooksPtr = makeMonitorHooks();
    hooksPtr->onRefreshComplete = [&] { refreshComplete.release(); };
    auto provider = AlsaProvider{hooksPtr};
    auto firstCalls = std::atomic{std::size_t{0U}};
    auto secondCalls = std::atomic{std::size_t{0U}};
    auto firstSub = Subscription{};
    auto secondSub = Subscription{};
    auto firstThread =
      std::jthread{[&]
                   {
                     firstSub = provider.subscribeDevices([&](std::vector<Device> const&)
                                                          { firstCalls.fetch_add(1U, std::memory_order_relaxed); });
                   }};
    auto secondThread =
      std::jthread{[&]
                   {
                     secondSub = provider.subscribeDevices([&](std::vector<Device> const&)
                                                           { secondCalls.fetch_add(1U, std::memory_order_relaxed); });
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

    REQUIRE(refreshComplete.try_acquire_for(kWaitTimeout));
    CHECK(firstCalls.load(std::memory_order_relaxed) == 1U);
    CHECK(secondCalls.load(std::memory_order_relaxed) == 2U);
    provider.shutdown();
  }

  TEST_CASE("AlsaProvider - reset removes a callback copied by monitor publication",
            "[audio][regression][alsa][concurrency]")
  {
    auto refreshComplete = std::binary_semaphore{0};
    auto hooksPtr = makeMonitorHooks();
    hooksPtr->onRefreshComplete = [&] { refreshComplete.release(); };
    auto provider = AlsaProvider{hooksPtr};
    auto cancelSecond = std::atomic_bool{false};
    auto firstCalls = std::atomic{std::size_t{0U}};
    auto secondCalls = std::atomic{std::size_t{0U}};
    auto secondSub = Subscription{};
    auto firstSub = provider.subscribeDevices(
      [&](std::vector<Device> const&)
      {
        firstCalls.fetch_add(1U, std::memory_order_relaxed);

        if (cancelSecond.load(std::memory_order_acquire))
        {
          secondSub.reset();
        }
      });
    secondSub = provider.subscribeDevices([&](std::vector<Device> const&)
                                          { secondCalls.fetch_add(1U, std::memory_order_relaxed); });
    REQUIRE(firstSub);
    REQUIRE(secondSub);
    cancelSecond.store(true, std::memory_order_release);
    REQUIRE(hooksPtr->requestRefresh);

    hooksPtr->requestRefresh();

    REQUIRE(refreshComplete.try_acquire_for(kWaitTimeout));
    CHECK(firstCalls.load(std::memory_order_relaxed) == 2U);
    CHECK(secondCalls.load(std::memory_order_relaxed) == 1U);
    provider.shutdown();
  }

  TEST_CASE("AlsaProvider - concurrent external shutdown callers share callback quiescence",
            "[audio][regression][alsa][concurrency]")
  {
    auto callbackEntered = std::binary_semaphore{0};
    auto releaseCallback = std::binary_semaphore{0};
    auto shutdownStarted = std::binary_semaphore{0};
    auto shutdownWait = std::binary_semaphore{0};
    auto firstReturned = std::binary_semaphore{0};
    auto secondReturned = std::binary_semaphore{0};
    auto hooksPtr = makeMonitorHooks();
    hooksPtr->onShutdownStarted = [&] { shutdownStarted.release(); };
    hooksPtr->onShutdownWait = [&] { shutdownWait.release(); };
    auto provider = AlsaProvider{hooksPtr};
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
    CHECK(provider.status().devices.empty());
  }

  TEST_CASE("AlsaProvider - callback shutdown returns while later external shutdown waits",
            "[audio][regression][alsa][concurrency]")
  {
    auto callbackShutdownReturned = std::binary_semaphore{0};
    auto releaseCallback = std::binary_semaphore{0};
    auto shutdownWait = std::binary_semaphore{0};
    auto externalReturned = std::binary_semaphore{0};
    auto monitorExited = std::binary_semaphore{0};
    auto startLateSubscription = std::binary_semaphore{0};
    auto lateSubscriptionAttempted = std::binary_semaphore{0};
    auto lateSubscriptionFinished = std::binary_semaphore{0};
    auto hooksPtr = makeMonitorHooks();
    hooksPtr->onShutdownWait = [&] { shutdownWait.release(); };
    hooksPtr->onMonitorExit = [&] { monitorExited.release(); };
    auto provider = AlsaProvider{hooksPtr};
    auto callbackCount = std::atomic{std::size_t{0U}};
    auto lateCalls = std::atomic{std::size_t{0U}};
    auto lateSub = Subscription{};
    auto lateSubscriber =
      std::jthread{[&]
                   {
                     startLateSubscription.acquire();
                     lateSub = provider.subscribeDevices([&](std::vector<Device> const&)
                                                         { lateCalls.fetch_add(1U, std::memory_order_relaxed); });
                     lateSubscriptionAttempted.release();
                   }};
    auto sub = provider.subscribeDevices(
      [&](std::vector<Device> const&)
      {
        if (callbackCount.fetch_add(1U, std::memory_order_relaxed) == 0U)
        {
          return;
        }

        provider.shutdown();
        callbackShutdownReturned.release();
        startLateSubscription.release();
        lateSubscriptionAttempted.acquire();
        lateSubscriptionFinished.release();
        releaseCallback.acquire();
      });
    REQUIRE(sub);
    REQUIRE(hooksPtr->requestRefresh);

    hooksPtr->requestRefresh();
    REQUIRE(callbackShutdownReturned.try_acquire_for(kWaitTimeout));
    REQUIRE(lateSubscriptionFinished.try_acquire_for(kWaitTimeout));
    auto externalShutdown = std::jthread{[&]
                                         {
                                           provider.shutdown();
                                           externalReturned.release();
                                         }};
    REQUIRE(shutdownWait.try_acquire_for(kWaitTimeout));

    CHECK_FALSE(externalReturned.try_acquire());
    releaseCallback.release();
    externalShutdown.join();
    lateSubscriber.join();
    REQUIRE(monitorExited.try_acquire_for(kWaitTimeout));
    CHECK(externalReturned.try_acquire());
    CHECK(callbackCount.load(std::memory_order_relaxed) == 2U);
    CHECK(lateCalls.load(std::memory_order_relaxed) == 0U);
    CHECK_FALSE(lateSub);
  }
} // namespace ao::audio::backend::test
