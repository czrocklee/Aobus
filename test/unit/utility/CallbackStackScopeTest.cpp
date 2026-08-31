// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/utility/CallbackStackScope.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <semaphore>
#include <thread>

namespace ao::utility::test
{
  TEST_CASE("CallbackStackScope - nested scopes retain every active identity", "[utility][unit][lifetime]")
  {
    auto firstIdentity = std::byte{};
    auto secondIdentity = std::byte{};

    CHECK_FALSE(CallbackStackScope::containsIdentity(&firstIdentity));
    CHECK_FALSE(CallbackStackScope::containsIdentity(&secondIdentity));

    {
      auto const firstScope = CallbackStackScope{&firstIdentity};
      CHECK(CallbackStackScope::containsIdentity(&firstIdentity));

      {
        auto const secondScope = CallbackStackScope{&secondIdentity};
        CHECK(CallbackStackScope::containsIdentity(&firstIdentity));
        CHECK(CallbackStackScope::containsIdentity(&secondIdentity));
      }

      CHECK(CallbackStackScope::containsIdentity(&firstIdentity));
      CHECK_FALSE(CallbackStackScope::containsIdentity(&secondIdentity));
    }

    CHECK_FALSE(CallbackStackScope::containsIdentity(&firstIdentity));
    CHECK_FALSE(CallbackStackScope::containsIdentity(&secondIdentity));
  }

  TEST_CASE("CallbackStackScope - repeated identity remains active until its outer scope exits",
            "[utility][unit][lifetime]")
  {
    auto identity = std::byte{};

    {
      auto const outerScope = CallbackStackScope{&identity};

      {
        auto const innerScope = CallbackStackScope{&identity};
        CHECK(CallbackStackScope::containsIdentity(&identity));
      }

      CHECK(CallbackStackScope::containsIdentity(&identity));
    }

    CHECK_FALSE(CallbackStackScope::containsIdentity(&identity));
  }

  TEST_CASE("CallbackStackScope - identities are isolated between threads", "[utility][unit][lifetime][concurrency]")
  {
    auto mainIdentity = std::byte{};
    auto workerIdentity = std::byte{};
    auto workerReady = std::binary_semaphore{0};
    auto releaseWorker = std::binary_semaphore{0};
    auto workerSeesMain = std::atomic_bool{true};
    auto workerSeesOwn = std::atomic_bool{false};
    auto const mainScope = CallbackStackScope{&mainIdentity};
    auto worker = std::jthread{
      [&]
      {
        workerSeesMain.store(CallbackStackScope::containsIdentity(&mainIdentity), std::memory_order_relaxed);
        auto const workerScope = CallbackStackScope{&workerIdentity};
        workerSeesOwn.store(CallbackStackScope::containsIdentity(&workerIdentity), std::memory_order_relaxed);
        workerReady.release();
        releaseWorker.acquire();
      }};

    workerReady.acquire();
    auto const mainSeesWorker = CallbackStackScope::containsIdentity(&workerIdentity);
    auto const mainSeesOwn = CallbackStackScope::containsIdentity(&mainIdentity);
    releaseWorker.release();
    worker.join();

    CHECK_FALSE(workerSeesMain.load(std::memory_order_relaxed));
    CHECK(workerSeesOwn.load(std::memory_order_relaxed));
    CHECK_FALSE(mainSeesWorker);
    CHECK(mainSeesOwn);
  }
} // namespace ao::utility::test
