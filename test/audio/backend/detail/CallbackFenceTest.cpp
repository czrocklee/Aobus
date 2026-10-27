// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/CallbackFence.h"

#include "test/fatal/AudioFatalProbeProtocol.h"
#include "test/fatal/ProbeProcess.h"
#include <ao/utility/ScopedRegistration.h>
#include <ao/utility/ThreadName.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <semaphore>
#include <stop_token>
#include <thread>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("CallbackFence - close waits for an admitted callback and prevents a late entrant", "[concurrency]")
  {
    auto fence = CallbackFence{};
    auto entered = std::binary_semaphore{0};
    auto release = std::binary_semaphore{0};
    auto closeReturned = std::atomic<bool>{false};
    auto enteredFence = std::atomic<bool>{false};
    fence.open();

    auto callbackThread = std::jthread{};
    auto closeThread = std::jthread{};
    auto cleanup = utility::ScopedRegistration{[&]
                                               {
                                                 release.release();

                                                 if (callbackThread.joinable())
                                                 {
                                                   callbackThread.join();
                                                 }

                                                 if (closeThread.joinable())
                                                 {
                                                   closeThread.join();
                                                 }
                                               }};
    callbackThread = std::jthread{[&]
                                  {
                                    enteredFence.store(fence.tryEnter(), std::memory_order_release);
                                    entered.release();
                                    release.acquire();

                                    if (enteredFence.load(std::memory_order_acquire))
                                    {
                                      fence.leave();
                                    }
                                  }};
    REQUIRE(entered.try_acquire_for(std::chrono::seconds{5}));
    REQUIRE(enteredFence.load(std::memory_order_acquire));

    closeThread = std::jthread{[&]
                               {
                                 fence.closeAndWait();
                                 closeReturned.store(true, std::memory_order_release);
                               }};

    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};

    while (fence.isOpen() && std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::yield();
    }

    REQUIRE_FALSE(fence.isOpen());
    CHECK_FALSE(closeReturned.load(std::memory_order_acquire));
    auto const lateEntry = fence.tryEnter();

    if (lateEntry)
    {
      fence.leave();
    }

    CHECK_FALSE(lateEntry);

    cleanup.reset();
    CHECK(closeReturned.load(std::memory_order_acquire));
  }

  TEST_CASE("CallbackFence - a new run can reopen admission")
  {
    auto fence = CallbackFence{};
    CHECK_FALSE(fence.tryEnter());
    fence.open();
    REQUIRE(fence.tryEnter());
    fence.leave();
    fence.closeAndWait();
    CHECK_FALSE(fence.isOpen());
    fence.open();
    CHECK(fence.isOpen());
    REQUIRE(fence.tryEnter());
    fence.leave();
    fence.closeAndWait();
    CHECK_FALSE(fence.tryEnter());
  }

  TEST_CASE("CallbackFence - listener removal can separate close from quiescence", "[concurrency]")
  {
    auto fence = CallbackFence{};
    fence.open();
    REQUIRE(fence.tryEnter());
    fence.close();
    CHECK_FALSE(fence.isOpen());
    fence.leave();
    fence.wait();
    CHECK_FALSE(fence.tryEnter());
  }

  TEST_CASE("CallbackFence - closed rejection leaves the interval reusable for publication")
  {
    auto fence = CallbackFence{};
    std::uint32_t payload = 0U;

    // A native callback equivalent can arrive while admission is still closed.
    CHECK_FALSE(fence.isOpen());
    CHECK_FALSE(fence.tryEnter());
    CHECK_FALSE(fence.tryEnter());
    CHECK_FALSE(fence.isOpen());

    // Those rejections do not borrow a slot, so the quiescent interval can publish and reopen.
    payload = 1U;
    fence.open();
    REQUIRE(fence.isOpen());
    REQUIRE(fence.tryEnter());
    CHECK(payload == 1U);
    fence.leave();

    fence.closeAndWait();
    CHECK_FALSE(fence.isOpen());
    CHECK_FALSE(fence.tryEnter());

    payload = 2U;
    fence.open();
    REQUIRE(fence.tryEnter());
    CHECK(payload == 2U);
    fence.leave();
    fence.closeAndWait();
    CHECK_FALSE(fence.tryEnter());
  }

  TEST_CASE("CallbackFence - concurrent reopen races continuous closed native attempts", "[concurrency]")
  {
    constexpr std::uint32_t kNativeAttemptThreads = 4U;
    constexpr std::uint32_t kReopenCount = 10000U;

    // A closed tryEnter returns only after that attempt finishes, so the public API cannot park
    // the historical fetch_add/undo window inside the call. This case exercises the real reopen
    // race and does not deterministically fail that older window. tryEnter's CAS increments only
    // while the fence is open, and that is what closes the window.
    auto fence = CallbackFence{};
    std::uint32_t payload = 0U;
    auto admittedCount = std::atomic<std::uint64_t>{0};
    auto rejectedCount = std::atomic<std::uint64_t>{0};
    auto invalidPayloadCount = std::atomic<std::uint64_t>{0};
    auto invalidPayloadValue = std::atomic<std::uint32_t>{0};

    // Declared after the shared state so unwind joins workers before that state is destroyed.
    // Stop then join settles admitted slots: a worker never waits while it still holds a borrow.
    auto workers = std::array<std::jthread, kNativeAttemptThreads>{};
    auto stopWorkers = utility::ScopedRegistration{[&workers]
                                                   {
                                                     for (auto& worker : workers)
                                                     {
                                                       if (worker.joinable())
                                                       {
                                                         worker.request_stop();
                                                       }
                                                     }

                                                     for (auto& worker : workers)
                                                     {
                                                       if (worker.joinable())
                                                       {
                                                         worker.join();
                                                       }
                                                     }
                                                   }};

    for (auto& worker : workers)
    {
      worker = std::jthread{[&](std::stop_token stopToken)
                            {
                              setCurrentThreadName("ao-fence-native");

                              while (!stopToken.stop_requested())
                              {
                                if (fence.tryEnter())
                                {
                                  std::uint32_t const observed = payload;
                                  fence.leave();

                                  if (observed == 0U || observed > kReopenCount)
                                  {
                                    invalidPayloadCount.fetch_add(1, std::memory_order_relaxed);
                                    invalidPayloadValue.store(observed, std::memory_order_relaxed);
                                  }

                                  admittedCount.fetch_add(1, std::memory_order_release);
                                }
                                else
                                {
                                  rejectedCount.fetch_add(1, std::memory_order_release);
                                }
                              }
                            }};
    }

    // Counter change is the proof. The deadline only stops a hang and is not race evidence.
    auto const waitForCountChange = [](std::atomic<std::uint64_t> const& count, std::uint64_t const baseline)
    {
      auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};

      while (count.load(std::memory_order_acquire) == baseline && std::chrono::steady_clock::now() < deadline)
      {
        std::this_thread::yield();
      }
    };

    bool observedClosedActivity = true;
    std::uint32_t completedReopens = 0U;

    // Close, wait, and open stay on this thread and stay serialized.
    for (std::uint32_t round = 0U; round < kReopenCount; ++round)
    {
      fence.closeAndWait();
      std::uint64_t const rejectionBaseline = rejectedCount.load(std::memory_order_acquire);
      waitForCountChange(rejectedCount, rejectionBaseline);

      if (rejectedCount.load(std::memory_order_acquire) == rejectionBaseline)
      {
        observedClosedActivity = false;
        break;
      }

      payload = round + 1U;
      fence.open();
      ++completedReopens;
    }

    bool observedAdmission = admittedCount.load(std::memory_order_acquire) > 0U;

    if (!observedAdmission && observedClosedActivity && completedReopens == kReopenCount)
    {
      // The last open is still current, so one admitted read is an observation rather than a sleep.
      waitForCountChange(admittedCount, 0U);
      observedAdmission = admittedCount.load(std::memory_order_acquire) > 0U;
    }

    fence.closeAndWait();
    payload = 0U;
    stopWorkers.reset();

    REQUIRE(observedClosedActivity);
    CHECK(completedReopens == kReopenCount);
    CHECK(observedAdmission);
    CHECK(admittedCount.load(std::memory_order_acquire) > 0U);
    CHECK(rejectedCount.load(std::memory_order_acquire) > 0U);
    CHECK(invalidPayloadCount.load(std::memory_order_acquire) == 0U);
    CHECK(invalidPayloadValue.load(std::memory_order_acquire) == 0U);
    CHECK_FALSE(fence.isOpen());
  }

  TEST_CASE("CallbackFence - reopening before callbacks quiesce aborts the fatal probe child")
  {
    // Two children drive only the public single-threaded fence API in the
    // fatal probe process: one reopens a fence that is already open, and one
    // closes a fence while it holds an admitted borrow and reopens before the
    // borrow leaves. Both are deterministic open preconditions, not the
    // historical concurrent fetch_add/undo race window covered by the stress
    // case above. A failed setup or a surviving reopen makes the child exit
    // nonfatally without leave()/wait() cleanup, and this case fails through
    // the same 15s probe oracle as the other fatal registrations.
    constexpr auto kProbeTimeout = std::chrono::seconds{15};
    auto const executablePath = ao::test::siblingProbeExecutablePath("ao_audio_fatal_probe");
    REQUIRE_FALSE(executablePath.empty());

    for (auto const& expectation : audio::test::callbackFenceFatalProbeExpectations())
    {
      INFO("probe: " << expectation.scenario);
      auto const result = ao::test::runProbeProcess(executablePath, expectation.scenario, kProbeTimeout);

      REQUIRE(result.started);
      CHECK(result.launchError.empty());
      CHECK_FALSE(result.timedOut);
      CHECK(result.hasFatalTermination());
      CHECK(result.hasPlatformAbort());
      CHECK(result.standardError.contains("AOBUS_FATAL"));
      CHECK(result.standardError.contains(expectation.category));
      CHECK(result.standardError.contains(expectation.context));
      CHECK(result.standardError.contains("source="));
      CHECK(result.standardError.contains("CallbackFence.cpp:"));
    }
  }
} // namespace ao::audio::backend::detail::test
