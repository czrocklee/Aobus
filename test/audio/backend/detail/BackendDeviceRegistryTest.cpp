// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/BackendDeviceRegistry.h"

#include <ao/audio/Device.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <semaphore>
#include <thread>
#include <utility>
#include <vector>

namespace ao::audio::backend::detail::test
{
  namespace
  {
    constexpr auto kWaitTimeout = std::chrono::seconds{5};
    constexpr auto kCallbackTimeout = std::chrono::seconds{15};

    class CallbackPause final
    {
    public:
      void park()
      {
        _entered.release();
        _resumed = _resume.try_acquire_for(kCallbackTimeout);
      }

      bool tryWaitForEntry() { return _entered.try_acquire_for(kWaitTimeout); }
      void resume() { _resume.release(); }
      bool wasResumed() const { return _resumed; }

    private:
      std::binary_semaphore _entered{0};
      std::binary_semaphore _resume{0};
      bool _resumed = false;
    };
  } // namespace

  TEST_CASE("BackendDeviceRegistry - publishes snapshots and honors cancellation")
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

  TEST_CASE("BackendDeviceRegistry - delivery preserves callable state and its destructor can query the registry")
  {
    struct DeliveryRecorder final
    {
      std::reference_wrapper<std::vector<std::size_t>> deliveries;
      utility::ScopedRegistration onRelease;
    };

    auto registry = BackendDeviceRegistry{};
    auto optRetiredSnapshot = std::optional<std::vector<Device>>{};
    auto deliveries = std::vector<std::size_t>{};
    auto recorderPtr = std::make_shared<DeliveryRecorder>(DeliveryRecorder{
      .deliveries = std::ref(deliveries),
      .onRelease = utility::ScopedRegistration{[&] { optRetiredSnapshot = registry.snapshot(); }},
    });
    auto sub = registry.subscribe(
      [recorderPtr = std::move(recorderPtr), count = std::size_t{0}](std::vector<Device> const&) mutable
      { recorderPtr->deliveries.get().push_back(++count); });
    REQUIRE(sub);
    auto const devices = std::vector<Device>{{.id = DeviceId{"uid-a"}}};
    registry.publish(devices);
    CHECK((deliveries == std::vector<std::size_t>{1U, 2U}));
    REQUIRE_FALSE(optRetiredSnapshot);

    SECTION("registration reset")
    {
      sub.reset();
      REQUIRE(optRetiredSnapshot);
      CHECK(*optRetiredSnapshot == devices);
    }

    SECTION("shutdown")
    {
      registry.shutdown();
      REQUIRE(optRetiredSnapshot);
      CHECK(optRetiredSnapshot->empty());
    }
  }

  TEST_CASE("BackendDeviceRegistry - shutdown closes admission and clears status")
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

  TEST_CASE("BackendDeviceRegistry - one callback may cancel another copied subscriber", "[concurrency]")
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

  TEST_CASE("BackendDeviceRegistry - reentrant publication supersedes the older snapshot")
  {
    auto registry = BackendDeviceRegistry{};
    auto const older = std::vector<Device>{{.id = DeviceId{"uid-a"}}};
    auto const newer = std::vector<Device>{{.id = DeviceId{"uid-b"}}};
    auto observed = std::vector<std::vector<Device>>{};
    auto firstSub = registry.subscribe(
      [&](std::vector<Device> const& snapshot)
      {
        if (snapshot == older)
        {
          registry.publish(newer);
        }
      });
    auto secondSub = registry.subscribe([&](std::vector<Device> const& snapshot) { observed.push_back(snapshot); });
    REQUIRE(firstSub);
    REQUIRE(secondSub);

    registry.publish(older);

    CHECK((observed == std::vector<std::vector<Device>>{{}, newer}));
    CHECK(registry.snapshot() == newer);
  }

  TEST_CASE("BackendDeviceRegistry - external reset releases an observer before its turn to run", "[concurrency]")
  {
    auto registry = BackendDeviceRegistry{};
    auto const devices = std::vector<Device>{{.id = DeviceId{"uid-a"}}};
    auto pause = CallbackPause{};
    auto firstSnapshots = std::vector<std::vector<Device>>{};
    auto secondSnapshots = std::vector<std::vector<Device>>{};
    auto firstSub = registry.subscribe(
      [&](std::vector<Device> const& snapshot)
      {
        firstSnapshots.push_back(snapshot);

        if (snapshot == devices)
        {
          pause.park();
        }
      });
    auto receiverPtr = std::make_shared<BackendDeviceRegistry::Callback>([&](std::vector<Device> const& snapshot)
                                                                         { secondSnapshots.push_back(snapshot); });
    auto const weakReceiverPtr = std::weak_ptr{receiverPtr};
    auto secondSub = registry.subscribe([receiverPtr = std::move(receiverPtr)](std::vector<Device> const& snapshot)
                                        { (*receiverPtr)(snapshot); });
    REQUIRE(firstSub);
    REQUIRE(secondSub);
    REQUIRE_FALSE(weakReceiverPtr.expired());

    auto resetReturned = std::binary_semaphore{0};
    auto resetThread = std::jthread{};
    auto publisher = std::jthread{[&] { registry.publish(devices); }};
    // Unpark before joining either thread, including during assertion unwinding.
    auto resumeOnExit = utility::ScopedRegistration{[&] { pause.resume(); }};
    REQUIRE(pause.tryWaitForEntry());
    resetThread = std::jthread{[&]
                               {
                                 secondSub.reset();
                                 resetReturned.release();
                               }};
    bool const resetWhileParked = resetReturned.try_acquire_for(kWaitTimeout);
    bool const receiverExpiredWhileParked = weakReceiverPtr.expired();

    resumeOnExit.reset();
    publisher.join();
    resetThread.join();

    REQUIRE(pause.wasResumed());
    CHECK(resetWhileParked);
    CHECK(receiverExpiredWhileParked);
    CHECK((firstSnapshots == std::vector<std::vector<Device>>{{}, devices}));
    CHECK((secondSnapshots == std::vector<std::vector<Device>>{{}}));
  }

  TEST_CASE("BackendDeviceRegistry - external reset waits for its active observer", "[concurrency]")
  {
    auto registry = BackendDeviceRegistry{};
    auto const devices = std::vector<Device>{{.id = DeviceId{"uid-a"}}};
    auto pause = CallbackPause{};
    auto leavingCallback = std::atomic_bool{false};
    auto snapshots = std::vector<std::vector<Device>>{};
    auto sub = registry.subscribe(
      [&](std::vector<Device> const& snapshot)
      {
        snapshots.push_back(snapshot);

        if (snapshot == devices)
        {
          pause.park();
          leavingCallback.store(true);
        }
      });
    REQUIRE(sub);

    auto resetStarted = std::binary_semaphore{0};
    auto resetReturned = std::binary_semaphore{0};
    bool resetObservedCallbackExit = false;
    auto resetThread = std::jthread{};
    auto shutdownThread = std::jthread{};
    auto publisher = std::jthread{[&] { registry.publish(devices); }};
    auto resumeOnExit = utility::ScopedRegistration{[&] { pause.resume(); }};
    REQUIRE(pause.tryWaitForEntry());

    SECTION("registry remains open")
    {
      CHECK(registry.snapshot() == devices);
    }

    SECTION("concurrent shutdown has cleared the registry")
    {
      shutdownThread = std::jthread{[&] { registry.shutdown(); }};
      // Observe retirement through the public snapshot before starting reset.
      auto const deadline = std::chrono::steady_clock::now() + kWaitTimeout;

      while (!registry.snapshot().empty() && std::chrono::steady_clock::now() < deadline)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }

      REQUIRE(registry.snapshot().empty());
    }

    resetThread = std::jthread{[&]
                               {
                                 resetStarted.release();
                                 sub.reset();
                                 resetObservedCallbackExit = leavingCallback.load();
                                 resetReturned.release();
                               }};
    REQUIRE(resetStarted.try_acquire_for(kWaitTimeout));
    // Without an admission hook, give a broken reset an opportunity to return
    // while the callback is still parked, then check the observed exit order.
    bool const resetReturnedWhileParked = resetReturned.try_acquire_for(std::chrono::milliseconds{100});
    resumeOnExit.reset();
    publisher.join();
    resetThread.join();

    if (shutdownThread.joinable())
    {
      shutdownThread.join();
    }

    REQUIRE(pause.wasResumed());
    CHECK_FALSE(resetReturnedWhileParked);
    CHECK(resetObservedCallbackExit);
    registry.publish({{.id = DeviceId{"uid-b"}}});
    CHECK((snapshots == std::vector<std::vector<Device>>{{}, devices}));
  }
} // namespace ao::audio::backend::detail::test
