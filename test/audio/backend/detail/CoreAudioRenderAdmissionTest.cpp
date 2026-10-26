// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/CoreAudioRenderAdmission.h"

#include "lib/audio/backend/detail/CoreAudioControlMailbox.h"
#include "lib/audio/backend/detail/CoreAudioRunControl.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <latch>
#include <mutex>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("CoreAudioRenderAdmission - closed admission refuses entry and live scopes publish current faults")
  {
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<CoreAudioRenderAdmission::Scope>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<CoreAudioRenderAdmission::Scope>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<CoreAudioRenderAdmission::Scope>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<CoreAudioRenderAdmission::Scope>);

    auto control = CoreAudioRunControl{};
    auto mailbox = CoreAudioControlMailbox{};
    auto admission = CoreAudioRenderAdmission{};

    CHECK_FALSE(admission.tryEnter().has_value());

    auto const generation = control.current();
    admission.open(generation);

    auto optScope = admission.tryEnter();
    REQUIRE(optScope);
    CHECK(optScope->generation() == generation);
    CHECK(control.isCurrent(optScope->generation()));

    auto fault = CoreAudioFault::RenderBufferShape;
    auto supersededFault = CoreAudioFault::RenderFrameLimit;

    SECTION("buffer shape fault")
    {
      fault = CoreAudioFault::RenderBufferShape;
      supersededFault = CoreAudioFault::RenderTargetMissing;
    }

    SECTION("frame limit fault")
    {
      fault = CoreAudioFault::RenderFrameLimit;
      supersededFault = CoreAudioFault::RenderBufferShape;
    }

    SECTION("missing render target fault")
    {
      fault = CoreAudioFault::RenderTargetMissing;
      supersededFault = CoreAudioFault::RenderFrameLimit;
    }

    REQUIRE(optScope->tryPublishFault(control, mailbox, fault));

    // First pending cause wins within the interval: the second render fault of
    // the same generation must not replace the pending packet.
    CHECK_FALSE(optScope->tryPublishFault(control, mailbox, supersededFault));

    auto const optPacket = mailbox.takeFault();
    REQUIRE(optPacket);
    CHECK(optPacket->generation == generation);
    CHECK(optPacket->kind == fault);
    CHECK_FALSE(mailbox.takeFault().has_value());
  }

  TEST_CASE("CoreAudioRenderAdmission - stop retirement cannot retag an entry before fence closure", "[concurrency]")
  {
    auto control = CoreAudioRunControl{};
    auto mailbox = CoreAudioControlMailbox{};
    auto admission = CoreAudioRenderAdmission{};

    auto const admittedGeneration = control.current();
    admission.open(admittedGeneration);

    // Stop advances the control generation while admission is still open.
    auto const advancedScope = control.advance();
    CHECK(advancedScope.generation() == admittedGeneration + 1U);
    CHECK(control.current() == admittedGeneration + 1U);

    auto optScope = admission.tryEnter();
    REQUIRE(optScope);

    // The token was fixed when this interval opened: resampling current() here
    // would observe the newer generation instead, so only the captured interval
    // token may reach the mailbox, and it must be rejected.
    CHECK(optScope->generation() == admittedGeneration);
    CHECK_FALSE(optScope->tryPublishFault(control, mailbox, CoreAudioFault::RenderBufferShape));
    CHECK_FALSE(optScope->tryPublishFault(control, mailbox, CoreAudioFault::RenderFrameLimit));
    CHECK_FALSE(optScope->tryPublishFault(control, mailbox, CoreAudioFault::RenderTargetMissing));
    CHECK_FALSE(mailbox.takeFault().has_value());

    optScope.reset();
    admission.closeAndWait();
    CHECK_FALSE(admission.tryEnter().has_value());
  }

  TEST_CASE("CoreAudioRenderAdmission - delayed entry across reopening captures the new interval", "[concurrency]")
  {
    auto control = CoreAudioRunControl{};
    auto mailbox = CoreAudioControlMailbox{};
    auto admission = CoreAudioRenderAdmission{};

    auto const firstGeneration = control.current();
    admission.open(firstGeneration);

    {
      auto optFirstScope = admission.tryEnter();
      REQUIRE(optFirstScope);
      CHECK(optFirstScope->generation() == firstGeneration);
    }

    admission.closeAndWait();
    auto const secondGeneration = control.advance().generation();
    admission.open(secondGeneration);

    // The entry arrives after the interval was replaced: it must capture the
    // actual new token at admission, not a stale pre-reopen sample.
    auto optScope = admission.tryEnter();
    REQUIRE(optScope);
    CHECK(optScope->generation() == secondGeneration);

    REQUIRE(optScope->tryPublishFault(control, mailbox, CoreAudioFault::RenderFrameLimit));
    auto const optPacket = mailbox.takeFault();
    REQUIRE(optPacket);
    CHECK(optPacket->generation == secondGeneration);
    CHECK(optPacket->kind == CoreAudioFault::RenderFrameLimit);
  }

  TEST_CASE("CoreAudioRenderAdmission - pause resume preserves identity and a new run replaces it")
  {
    auto control = CoreAudioRunControl{};
    auto mailbox = CoreAudioControlMailbox{};
    auto admission = CoreAudioRenderAdmission{};

    auto const pausedGeneration = control.current();
    admission.open(pausedGeneration);

    {
      auto optScope = admission.tryEnter();
      REQUIRE(optScope);
      CHECK(optScope->generation() == pausedGeneration);
    }

    // Pause closes and quiesces admission without advancing the run: resumed
    // current-interval faults must not be blanket suppressed.
    admission.closeAndWait();
    admission.open(pausedGeneration);

    {
      auto optScope = admission.tryEnter();
      REQUIRE(optScope);
      CHECK(optScope->generation() == pausedGeneration);
      REQUIRE(optScope->tryPublishFault(control, mailbox, CoreAudioFault::RenderTargetMissing));
    }

    auto const optResumedPacket = mailbox.takeFault();
    REQUIRE(optResumedPacket);
    CHECK(optResumedPacket->generation == pausedGeneration);
    CHECK(optResumedPacket->kind == CoreAudioFault::RenderTargetMissing);

    // Stop replaces the interval; the previous scopes have already settled.
    auto const replacementGeneration = control.advance().generation();
    admission.closeAndWait();
    admission.open(replacementGeneration);

    auto optScope = admission.tryEnter();
    REQUIRE(optScope);
    CHECK(optScope->generation() == replacementGeneration);
    REQUIRE(optScope->tryPublishFault(control, mailbox, CoreAudioFault::RenderBufferShape));

    auto const optReplacementPacket = mailbox.takeFault();
    REQUIRE(optReplacementPacket);
    CHECK(optReplacementPacket->generation == replacementGeneration);
    CHECK(optReplacementPacket->kind == CoreAudioFault::RenderBufferShape);
  }

  TEST_CASE(
    "CoreAudioRenderAdmission - moving a scope transfers the single fence leave and leaves the moved-from scope inert")
  {
    auto control = CoreAudioRunControl{};
    auto mailbox = CoreAudioControlMailbox{};
    auto admission = CoreAudioRenderAdmission{};

    auto const generation = control.current();
    admission.open(generation);

    auto optSourceScope = admission.tryEnter();
    REQUIRE(optSourceScope);

    {
      auto movedScope = std::move(*optSourceScope);
      CHECK(movedScope.generation() == generation);

      // Destroying the moved-from scope must not return a second borrow.
      optSourceScope.reset();

      REQUIRE(movedScope.tryPublishFault(control, mailbox, CoreAudioFault::RenderBufferShape));
      auto const optPacket = mailbox.takeFault();
      REQUIRE(optPacket);
      CHECK(optPacket->generation == generation);
      CHECK(optPacket->kind == CoreAudioFault::RenderBufferShape);

      // Exactly one borrow is outstanding: admission still admits entries.
      auto optProbeScope = admission.tryEnter();
      REQUIRE(optProbeScope);
    } // The moved scope ends here and returns the single borrow.

    // Quiescence completes only if the fence counter returned to exactly zero,
    // and the settled interval reopens for the next generation.
    admission.closeAndWait();
    auto const nextGeneration = control.advance().generation();
    admission.open(nextGeneration);

    auto optScope = admission.tryEnter();
    REQUIRE(optScope);
    CHECK(optScope->generation() == nextGeneration);
  }

  TEST_CASE("CoreAudioRenderAdmission - retired scopes cannot publish while close waits", "[concurrency]")
  {
    auto control = CoreAudioRunControl{};
    auto mailbox = CoreAudioControlMailbox{};
    auto admission = CoreAudioRenderAdmission{};

    auto const admittedGeneration = control.current();
    admission.open(admittedGeneration);

    // All shared state is declared before the threads, so an assertion unwind
    // stops and joins them before this state is destroyed. The closer is
    // declared first so unwind stops the callback before joining the closer.
    auto admittedLatch = std::latch{1};
    auto publishedLatch = std::latch{1};
    auto releaseMutex = std::mutex{};
    auto releaseSignal = std::condition_variable_any{};
    std::uint32_t releasePhase = 0U;
    auto admittedResult = std::atomic<bool>{false};
    auto capturedGeneration = std::atomic<std::uint64_t>{0};
    auto publishedBufferShape = std::atomic<bool>{true};
    auto publishedFrameLimit = std::atomic<bool>{true};
    auto publishedTargetMissing = std::atomic<bool>{true};
    auto closeReturned = std::atomic<bool>{false};

    auto closer = std::jthread{};
    auto callback = std::jthread{
      [&](std::stop_token stopToken)
      {
        auto optScope = admission.tryEnter();
        admittedResult.store(optScope.has_value(), std::memory_order_release);

        if (optScope)
        {
          capturedGeneration.store(optScope->generation(), std::memory_order_release);
        }

        admittedLatch.count_down();

        if (!optScope)
        {
          return;
        }

        {
          auto lock = std::unique_lock{releaseMutex};

          if (!releaseSignal.wait(lock, stopToken, [&releasePhase] { return releasePhase >= 1U; }))
          {
            return;
          }
        }

        publishedBufferShape.store(
          optScope->tryPublishFault(control, mailbox, CoreAudioFault::RenderBufferShape), std::memory_order_release);
        publishedFrameLimit.store(
          optScope->tryPublishFault(control, mailbox, CoreAudioFault::RenderFrameLimit), std::memory_order_release);
        publishedTargetMissing.store(
          optScope->tryPublishFault(control, mailbox, CoreAudioFault::RenderTargetMissing), std::memory_order_release);
        publishedLatch.count_down();

        {
          auto lock = std::unique_lock{releaseMutex};

          if (!releaseSignal.wait(lock, stopToken, [&releasePhase] { return releasePhase >= 2U; }))
          {
            return;
          }
        }
      }};

    admittedLatch.wait();
    REQUIRE(admittedResult.load(std::memory_order_acquire));
    REQUIRE(capturedGeneration.load(std::memory_order_acquire) == admittedGeneration);

    // Stop advances the control generation only after the callback was
    // admitted, before its scope has ended.
    auto const advancedScope = control.advance();
    CHECK(advancedScope.generation() == admittedGeneration + 1U);
    CHECK(control.current() == admittedGeneration + 1U);

    closer = std::jthread{[&]
                          {
                            admission.closeAndWait();
                            closeReturned.store(true, std::memory_order_release);
                          }};

    // A refused probe while the callback still holds its scope proves the
    // closer has closed admission: only closeAndWait closes it, and its wait
    // cannot return until the scope ends. The deadline guards against hangs
    // only; the refused entry is the proof.
    bool probeClosed = false;
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};

    while (std::chrono::steady_clock::now() < deadline)
    {
      if (!admission.tryEnter())
      {
        probeClosed = true;
        break;
      }

      std::this_thread::yield();
    }

    REQUIRE(probeClosed);
    CHECK_FALSE(closeReturned.load(std::memory_order_acquire));

    {
      auto lock = std::scoped_lock{releaseMutex};
      releasePhase = 1U;
    }
    releaseSignal.notify_all();

    publishedLatch.wait();

    // The callback published while closeAndWait was still waiting for its
    // scope, and every render fault kind was rejected as stale.
    CHECK_FALSE(closeReturned.load(std::memory_order_acquire));
    CHECK_FALSE(publishedBufferShape.load(std::memory_order_acquire));
    CHECK_FALSE(publishedFrameLimit.load(std::memory_order_acquire));
    CHECK_FALSE(publishedTargetMissing.load(std::memory_order_acquire));

    {
      auto lock = std::scoped_lock{releaseMutex};
      releasePhase = 2U;
    }
    releaseSignal.notify_all();

    callback.join();
    closer.join();
    CHECK(closeReturned.load(std::memory_order_acquire));
    CHECK_FALSE(mailbox.takeFault().has_value());
  }

  TEST_CASE("CoreAudioRenderAdmission - closeAndWait waits for a held scope and the settled admission reopens",
            "[concurrency]")
  {
    auto control = CoreAudioRunControl{};
    auto admission = CoreAudioRenderAdmission{};

    auto const generation = control.current();
    admission.open(generation);

    // All shared state is declared before the threads. The closer is declared
    // first so an unwind stops the holder before joining the closer.
    auto admittedLatch = std::latch{1};
    auto releaseMutex = std::mutex{};
    auto releaseSignal = std::condition_variable_any{};
    std::uint32_t releasePhase = 0U;
    auto admittedResult = std::atomic<bool>{false};
    auto closeReturned = std::atomic<bool>{false};

    auto closer = std::jthread{};
    auto holder =
      std::jthread{[&](std::stop_token stopToken)
                   {
                     auto optScope = admission.tryEnter();
                     admittedResult.store(optScope.has_value(), std::memory_order_release);
                     admittedLatch.count_down();

                     if (!optScope)
                     {
                       return;
                     }

                     auto lock = std::unique_lock{releaseMutex};

                     if (!releaseSignal.wait(lock, stopToken, [&releasePhase] { return releasePhase >= 1U; }))
                     {
                       return;
                     }
                   }}; // The held scope ends here.

    admittedLatch.wait();
    REQUIRE(admittedResult.load(std::memory_order_acquire));

    // The closer starts only after the holder was admitted, so its close()
    // cannot race the admission.
    closer = std::jthread{[&]
                          {
                            admission.closeAndWait();
                            closeReturned.store(true, std::memory_order_release);
                          }};

    // A refused entry probe while the holder keeps the fence occupied proves
    // the closer has closed admission and its wait cannot return. The deadline
    // guards against hangs only; the refused entry is the contention proof.
    bool probeClosed = false;
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};

    while (std::chrono::steady_clock::now() < deadline)
    {
      if (!admission.tryEnter())
      {
        probeClosed = true;
        break;
      }

      std::this_thread::yield();
    }

    REQUIRE(probeClosed);
    CHECK_FALSE(closeReturned.load(std::memory_order_acquire));

    {
      auto lock = std::scoped_lock{releaseMutex};
      releasePhase = 1U;
    }
    releaseSignal.notify_all();

    holder.join();
    closer.join();
    CHECK(closeReturned.load(std::memory_order_acquire));

    // The settled admission is reusable: a fresh interval opens and admits.
    auto const nextGeneration = control.advance().generation();
    admission.open(nextGeneration);
    auto optScope = admission.tryEnter();
    REQUIRE(optScope);
    CHECK(optScope->generation() == nextGeneration);
  }
} // namespace ao::audio::backend::detail::test
