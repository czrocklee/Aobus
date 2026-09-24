// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/CallbackFence.h"

#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <semaphore>
#include <thread>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("CallbackFence - close waits for an admitted callback and prevents a late entrant",
            "[audio][unit][callback-fence][concurrency]")
  {
    auto fence = CallbackFence{};
    auto entered = std::binary_semaphore{0};
    auto release = std::binary_semaphore{0};
    auto closeReturned = std::atomic{false};
    auto enteredFence = std::atomic{false};
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

  TEST_CASE("CallbackFence - a new run can reopen admission", "[audio][unit][callback-fence]")
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

  TEST_CASE("CallbackFence - listener removal can separate close from quiescence",
            "[audio][unit][callback-fence][concurrency]")
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
} // namespace ao::audio::backend::detail::test
