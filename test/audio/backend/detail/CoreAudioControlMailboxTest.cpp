// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/CoreAudioControlMailbox.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <latch>
#include <mutex>
#include <stop_token>
#include <thread>

namespace ao::audio::backend::detail::test
{
  namespace
  {
    // Literal generations for the stale-listener schedules: an admitted
    // listener snapshots the older generation while the control loop advances
    // to the newer one.
    constexpr std::uint64_t kOldGeneration = 24;
    constexpr std::uint64_t kNewGeneration = 25;
  } // namespace

  TEST_CASE("CoreAudioControlMailbox - a delayed old device snapshot cannot supersede a published newer generation",
            "[audio][unit][coreaudio][concurrency]")
  {
    auto mailbox = CoreAudioControlMailbox{};

    // All shared state is declared before the listener thread, so an
    // assertion unwind requests stop and joins the thread before this
    // synchronization state is destroyed.
    auto generation = std::atomic<std::uint64_t>{kOldGeneration};
    auto snapshotCaptured = std::latch{1};
    auto stalePublished = std::latch{1};
    auto releaseMutex = std::mutex{};
    auto releaseSignal = std::condition_variable_any{};
    bool released = false;
    auto stalePublishSucceeded = std::atomic<bool>{false};

    auto listener =
      std::jthread{[&](std::stop_token stopToken)
                   {
                     auto const snapshot = generation.load(std::memory_order_acquire);
                     snapshotCaptured.count_down();

                     {
                       auto lock = std::unique_lock{releaseMutex};

                       if (!releaseSignal.wait(lock, stopToken, [&released] { return released; }))
                       {
                         // Stop was requested while unwinding; exit without publishing.
                         return;
                       }
                     }

                     stalePublishSucceeded.store(mailbox.tryPublishDeviceChange(snapshot), std::memory_order_release);
                     stalePublished.count_down();
                   }};

    snapshotCaptured.wait();
    REQUIRE(generation.load(std::memory_order_acquire) == kOldGeneration);
    generation.store(kNewGeneration, std::memory_order_release);
    REQUIRE(mailbox.tryPublishDeviceChange(kNewGeneration));

    {
      auto lock = std::scoped_lock{releaseMutex};
      released = true;
    }
    releaseSignal.notify_all();

    stalePublished.wait();
    REQUIRE_FALSE(stalePublishSucceeded.load(std::memory_order_acquire));
    REQUIRE(mailbox.takeDeviceChange() == kNewGeneration);
    CHECK(mailbox.takeDeviceChange() == 0);
  }

  TEST_CASE("CoreAudioControlMailbox - a delayed old fault cannot replace a newer pending fault",
            "[audio][unit][coreaudio][concurrency]")
  {
    auto mailbox = CoreAudioControlMailbox{};

    // All shared state is declared before the listener thread, so an
    // assertion unwind requests stop and joins the thread before this
    // synchronization state is destroyed.
    auto generation = std::atomic<std::uint64_t>{kOldGeneration};
    auto snapshotCaptured = std::latch{1};
    auto stalePublished = std::latch{1};
    auto releaseMutex = std::mutex{};
    auto releaseSignal = std::condition_variable_any{};
    bool released = false;
    auto stalePublishSucceeded = std::atomic<bool>{false};

    auto listener =
      std::jthread{[&](std::stop_token stopToken)
                   {
                     auto const snapshot = generation.load(std::memory_order_acquire);
                     snapshotCaptured.count_down();

                     {
                       auto lock = std::unique_lock{releaseMutex};

                       if (!releaseSignal.wait(lock, stopToken, [&released] { return released; }))
                       {
                         // Stop was requested while unwinding; exit without publishing.
                         return;
                       }
                     }

                     stalePublishSucceeded.store(
                       mailbox.tryPublishFault(snapshot, CoreAudioFault::DeviceLost), std::memory_order_release);
                     stalePublished.count_down();
                   }};

    snapshotCaptured.wait();
    REQUIRE(generation.load(std::memory_order_acquire) == kOldGeneration);
    generation.store(kNewGeneration, std::memory_order_release);
    REQUIRE(mailbox.tryPublishFault(kNewGeneration, CoreAudioFault::RenderBufferShape));

    {
      auto lock = std::scoped_lock{releaseMutex};
      released = true;
    }
    releaseSignal.notify_all();

    stalePublished.wait();
    REQUIRE_FALSE(stalePublishSucceeded.load(std::memory_order_acquire));

    auto const optFault = mailbox.takeFault();
    REQUIRE(optFault);
    CHECK(optFault->generation == kNewGeneration);
    CHECK(optFault->kind == CoreAudioFault::RenderBufferShape);
    CHECK_FALSE(mailbox.takeFault().has_value());
  }

  TEST_CASE("CoreAudioControlMailbox - a newer fault generation supersedes an older pending fault",
            "[audio][unit][coreaudio][concurrency]")
  {
    auto mailbox = CoreAudioControlMailbox{};
    REQUIRE(mailbox.tryPublishFault(kOldGeneration, CoreAudioFault::DeviceLost));
    REQUIRE(mailbox.tryPublishFault(kNewGeneration, CoreAudioFault::RenderBufferShape));

    auto const optFault = mailbox.takeFault();
    REQUIRE(optFault);
    CHECK(optFault->generation == kNewGeneration);
    CHECK(optFault->kind == CoreAudioFault::RenderBufferShape);
    CHECK_FALSE(mailbox.takeFault().has_value());
  }

  TEST_CASE("CoreAudioControlMailbox - the first fault kind wins within one pending generation",
            "[audio][unit][coreaudio]")
  {
    auto mailbox = CoreAudioControlMailbox{};
    REQUIRE(mailbox.tryPublishFault(kNewGeneration, CoreAudioFault::ChannelRouteChanged));
    CHECK_FALSE(mailbox.tryPublishFault(kNewGeneration, CoreAudioFault::DeviceLost));
    CHECK_FALSE(mailbox.tryPublishFault(kNewGeneration, CoreAudioFault::RenderFrameLimit));

    auto const optFault = mailbox.takeFault();
    REQUIRE(optFault);
    CHECK(optFault->generation == kNewGeneration);
    CHECK(optFault->kind == CoreAudioFault::ChannelRouteChanged);
  }

  TEST_CASE("CoreAudioControlMailbox - a consumed device generation can publish again while pending work coalesces",
            "[audio][unit][coreaudio]")
  {
    auto mailbox = CoreAudioControlMailbox{};
    REQUIRE(mailbox.tryPublishDeviceChange(kOldGeneration));
    CHECK_FALSE(mailbox.tryPublishDeviceChange(kOldGeneration)); // coalesced while still pending
    REQUIRE(mailbox.takeDeviceChange() == kOldGeneration);

    // The take cleared the slot, so the same generation may report again.
    REQUIRE(mailbox.tryPublishDeviceChange(kOldGeneration));
    REQUIRE(mailbox.takeDeviceChange() == kOldGeneration);
    CHECK(mailbox.takeDeviceChange() == 0);
  }

  TEST_CASE("CoreAudioControlMailbox - a consumed fault generation can publish a new pending cause",
            "[audio][unit][coreaudio]")
  {
    auto mailbox = CoreAudioControlMailbox{};
    REQUIRE(mailbox.tryPublishFault(kNewGeneration, CoreAudioFault::DeviceLost));
    auto const optFirst = mailbox.takeFault();
    REQUIRE(optFirst);
    CHECK(optFirst->generation == kNewGeneration);
    CHECK(optFirst->kind == CoreAudioFault::DeviceLost);

    REQUIRE(mailbox.tryPublishFault(kNewGeneration, CoreAudioFault::ChannelRouteChanged));
    auto const optNext = mailbox.takeFault();
    REQUIRE(optNext);
    CHECK(optNext->generation == kNewGeneration);
    CHECK(optNext->kind == CoreAudioFault::ChannelRouteChanged);
    CHECK_FALSE(mailbox.takeFault().has_value());
  }

  TEST_CASE("CoreAudioControlMailbox - consuming an idle mailbox yields no event", "[audio][unit][coreaudio]")
  {
    auto mailbox = CoreAudioControlMailbox{};
    CHECK(mailbox.takeDeviceChange() == 0);
    CHECK_FALSE(mailbox.takeFault().has_value());
    CHECK(mailbox.takeDeviceChange() == 0);
    CHECK_FALSE(mailbox.takeFault().has_value());
  }
} // namespace ao::audio::backend::detail::test
