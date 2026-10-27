// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/CoreAudioRunControl.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <latch>
#include <mutex>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("CoreAudioRunControl - scopes capture the current generation and advancement retires old tokens")
  {
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<CoreAudioRunControl::Scope>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<CoreAudioRunControl::Scope>);
    auto control = CoreAudioRunControl{};
    CHECK(control.current() == 1U);
    CHECK(control.isCurrent(1U));

    {
      auto const scope = control.lock();
      CHECK(scope.generation() == 1U);
      // Atomic observations must remain usable while the control gate is held.
      CHECK(control.current() == 1U);
      CHECK(control.isCurrent(scope.generation()));
    }

    {
      auto scope = control.advance();
      auto const movedScope = std::move(scope);
      CHECK(movedScope.generation() == 2U);
      CHECK(control.current() == 2U);
      CHECK_FALSE(control.isCurrent(1U));
    }

    CHECK_FALSE(control.tryCurrent(0U).has_value());
    CHECK_FALSE(control.tryCurrent(1U).has_value());

    {
      auto const optScope = control.tryCurrent(2U);
      REQUIRE(optScope);
      CHECK(optScope->generation() == 2U);
    }

    auto const nextScope = control.advance();
    CHECK(nextScope.generation() == 3U);
    CHECK(control.current() == 3U);
  }

  TEST_CASE("CoreAudioRunControl - a delayed recognized old handler cannot stop a newer run", "[concurrency]")
  {
    auto control = CoreAudioRunControl{};
    auto const oldGeneration = control.current();
    // These are native-action observations, not another implementation of a gate.
    bool runActive = false;
    bool renderAllowed = false;
    bool fenceOpen = false;
    bool nativeStarted = false;
    std::uint32_t nativeStopCount = 0U;
    auto recognized = std::latch{1};
    auto handled = std::latch{1};
    auto releaseMutex = std::mutex{};
    auto releaseSignal = std::condition_variable_any{};
    bool released = false;
    auto wasRecognized = std::atomic<bool>{false};
    auto wasRejected = std::atomic<bool>{false};

    bool faultHandler = false;

    SECTION("fault already consumed from the mailbox")
    {
      faultHandler = true;
    }

    SECTION("drain already consumed from the pending slot")
    {
      faultHandler = false;
    }

    // Shared state precedes the jthread. Stop-aware parking permits assertion
    // unwind to join it, and no main-thread Scope survives until that join.
    auto handler = std::jthread{[&](std::stop_token stopToken)
                                {
                                  wasRecognized.store(control.isCurrent(oldGeneration), std::memory_order_release);
                                  recognized.count_down();
                                  {
                                    auto lock = std::unique_lock{releaseMutex};

                                    if (!releaseSignal.wait(lock, stopToken, [&] { return released; }))
                                    {
                                      return;
                                    }
                                  }

                                  auto const optScope = control.tryCurrent(oldGeneration);
                                  wasRejected.store(!optScope, std::memory_order_release);

                                  if (optScope)
                                  {
                                    runActive = false;

                                    if (faultHandler)
                                    {
                                      renderAllowed = false;
                                    }

                                    fenceOpen = false;
                                    nativeStarted = false;
                                    ++nativeStopCount;
                                  }

                                  handled.count_down();
                                }};

    recognized.wait();
    REQUIRE(wasRecognized.load(std::memory_order_acquire));

    {
      auto const stopScope = control.advance();
      CHECK(stopScope.generation() == 2U);
      runActive = false;
      renderAllowed = false;
      fenceOpen = false;
      nativeStarted = false;
    }

    {
      auto const startScope = control.advance();
      CHECK(startScope.generation() == 3U);
      runActive = true;
      renderAllowed = true;
      fenceOpen = true;
      nativeStarted = true;
    }

    {
      auto const lock = std::scoped_lock{releaseMutex};
      released = true;
    }
    releaseSignal.notify_all();
    handled.wait();
    handler.join();

    CHECK(wasRejected.load(std::memory_order_acquire));
    CHECK(control.current() == 3U);
    CHECK(runActive);
    CHECK(renderAllowed);
    CHECK(fenceOpen);
    CHECK(nativeStarted);
    CHECK(nativeStopCount == 0U);
  }

  TEST_CASE("CoreAudioRunControl - an admitted handler finishes native stop before a newer run starts", "[concurrency]")
  {
    auto control = CoreAudioRunControl{};
    auto const oldGeneration = control.current();
    auto releaseMutex = std::mutex{};
    auto releaseSignal = std::condition_variable_any{};
    bool advanceRequested = false;
    auto advancing = std::latch{1};
    auto advanceFinished = std::atomic<bool>{false};
    auto actions = std::vector<std::uint64_t>{};
    bool nativeStarted = true;
    bool fenceOpen = true;
    std::uint64_t advancedGeneration = 0U;

    // Construct the thread before acquiring the Scope so assertion unwind
    // releases the gate before jthread destruction requests stop and joins.
    auto starter = std::jthread{[&](std::stop_token stopToken)
                                {
                                  {
                                    auto lock = std::unique_lock{releaseMutex};

                                    if (!releaseSignal.wait(lock, stopToken, [&] { return advanceRequested; }))
                                    {
                                      return;
                                    }
                                  }

                                  advancing.count_down();
                                  auto const scope = control.advance();
                                  advancedGeneration = scope.generation();
                                  nativeStarted = true;
                                  fenceOpen = true;
                                  actions.push_back(advancedGeneration);
                                  advanceFinished.store(true, std::memory_order_release);
                                }};

    {
      auto const optScope = control.tryCurrent(oldGeneration);
      REQUIRE(optScope);
      REQUIRE(optScope->generation() == 1U);
      {
        auto const lock = std::scoped_lock{releaseMutex};
        advanceRequested = true;
      }
      releaseSignal.notify_all();
      advancing.wait();

      // The scope, not elapsed time or a queue CAS, protects the old action.
      CHECK_FALSE(advanceFinished.load(std::memory_order_acquire));
      CHECK(control.current() == 1U);
      nativeStarted = false;
      fenceOpen = false;
      actions.push_back(optScope->generation());
      CHECK_FALSE(nativeStarted);
      CHECK_FALSE(fenceOpen);
    }

    starter.join();
    CHECK(advanceFinished.load(std::memory_order_acquire));
    CHECK(advancedGeneration == 2U);
    CHECK(control.current() == 2U);
    CHECK_FALSE(control.isCurrent(oldGeneration));
    CHECK(nativeStarted);
    CHECK(fenceOpen);
    REQUIRE(actions.size() == 2U);
    CHECK(actions[0] == 1U); // old native stop
    CHECK(actions[1] == 2U); // new native start
  }
} // namespace ao::audio::backend::detail::test
