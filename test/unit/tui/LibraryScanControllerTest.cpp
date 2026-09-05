// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/LibraryScanController.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/AudioIdentityIndex.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryJobs.h>
#include <ao/uimodel/library/task/LibraryScanOutcome.h>
#include <ao/uimodel/status/activity/ActivityPresentationText.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    std::unique_ptr<async::Executor> makeQueuedExecutor(rt::test::QueuedExecutor*& executor)
    {
      auto ownerPtr = std::make_unique<rt::test::QueuedExecutor>();
      executor = ownerPtr.get();
      return ownerPtr;
    }

    std::string lastMessage(rt::NotificationService& notifications)
    {
      auto const feed = notifications.feed();
      REQUIRE_FALSE(feed.entries.empty());
      return std::get<std::string>(feed.entries.back().message);
    }

    async::Task<uimodel::LibraryScanOutcome> waitThenOutcome(async::Runtime* const runtime,
                                                             uimodel::LibraryScanOutcome outcome,
                                                             std::stop_token const stopToken)
    {
      co_await runtime->sleepFor(std::chrono::seconds{30}, stopToken);
      co_return outcome;
    }

    struct ScanFixture final
    {
      ao::test::TempDir tempDir{};
      std::unique_ptr<rt::test::ControlledSleeper> sleeperPtr = std::make_unique<rt::test::ControlledSleeper>();
      rt::test::QueuedExecutor* executor = nullptr;
      std::unique_ptr<rt::AppRuntime> runtimePtr{
        rt::test::makeRuntime(tempDir, makeQueuedExecutor(executor), nullptr, sleeperPtr.get())};

      LibraryScanController makeController(LibraryScanController::ScanTask scan) const
      {
        return LibraryScanController{
          runtimePtr->async(), runtimePtr->notifications(), ao::test::englishMessageCatalog(), std::move(scan)};
      }
    };
  } // namespace

  TEST_CASE("LibraryScanController - a finished scan presents its real outcome", "[tui][unit][scan]")
  {
    auto fixture = ScanFixture{};
    auto controller = fixture.makeController(
      [](std::stop_token const) -> async::Task<uimodel::LibraryScanOutcome>
      {
        co_return uimodel::LibraryScanOutcome{
          .verdict = uimodel::LibraryScanVerdict::Complete,
          .summary = {.newCount = 2},
        };
      });

    controller.start();
    REQUIRE(fixture.executor->drainUntil([&] { return controller.phase() == LibraryScanController::Phase::Idle; }));

    CHECK(controller.phase() == LibraryScanController::Phase::Idle);
    CHECK(lastMessage(fixture.runtimePtr->notifications()) ==
          uimodel::formatLibraryScanMessage(
            ao::test::englishMessageCatalog(),
            {.verdict = uimodel::LibraryScanVerdict::Complete, .summary = {.newCount = 2}}));
  }

  TEST_CASE("LibraryScanController - start while running posts a transient already-running notice", "[tui][unit][scan]")
  {
    auto fixture = ScanFixture{};
    auto* const runtime = &fixture.runtimePtr->async();
    auto controller = fixture.makeController([runtime](std::stop_token const stopToken)
                                             { return waitThenOutcome(runtime, {}, stopToken); });

    controller.start();
    REQUIRE(fixture.sleeperPtr->waitForCallCount(1));
    CHECK(controller.phase() == LibraryScanController::Phase::Running);

    controller.start();
    CHECK(lastMessage(fixture.runtimePtr->notifications()) == "A library scan is already running");
    CHECK(fixture.runtimePtr->notifications().feed().entries.back().lifetime.kind() ==
          rt::NotificationLifetimeKind::Transient);

    controller.retire();
    REQUIRE(fixture.sleeperPtr->waitForCancellation(0));
    fixture.executor->drain();
    CHECK(controller.phase() == LibraryScanController::Phase::Retired);
  }

  TEST_CASE("LibraryScanController - start while cancelling posts a cancellation-in-progress notice",
            "[tui][unit][scan]")
  {
    auto fixture = ScanFixture{};
    auto* const runtime = &fixture.runtimePtr->async();
    auto controller = fixture.makeController([runtime](std::stop_token const stopToken)
                                             { return waitThenOutcome(runtime, {}, stopToken); });

    controller.start();
    REQUIRE(fixture.sleeperPtr->waitForCallCount(1));
    controller.cancel();
    CHECK(controller.phase() == LibraryScanController::Phase::Cancelling);

    controller.start();
    CHECK(lastMessage(fixture.runtimePtr->notifications()) == "Scan cancellation is already in progress");

    REQUIRE(fixture.sleeperPtr->waitForCancellation(0));
    REQUIRE(fixture.executor->drainUntil([&] { return controller.phase() == LibraryScanController::Phase::Idle; }));
  }

  TEST_CASE("LibraryScanController - cancellation is silent and returns to Idle", "[tui][unit][scan][concurrency]")
  {
    auto fixture = ScanFixture{};
    auto* const runtime = &fixture.runtimePtr->async();
    auto controller = fixture.makeController(
      [runtime](std::stop_token const stopToken)
      { return waitThenOutcome(runtime, {.verdict = uimodel::LibraryScanVerdict::Complete}, stopToken); });

    controller.start();
    REQUIRE(fixture.sleeperPtr->waitForCallCount(1));
    controller.cancel();
    REQUIRE(fixture.sleeperPtr->waitForCancellation(0));
    REQUIRE(fixture.executor->drainUntil([&] { return controller.phase() == LibraryScanController::Phase::Idle; }));

    CHECK(fixture.runtimePtr->notifications().feed().entries.empty());
  }

  TEST_CASE("LibraryScanController - a real outcome that wins the cancel race is still presented",
            "[tui][unit][scan][concurrency]")
  {
    auto fixture = ScanFixture{};
    auto controller = fixture.makeController(
      [](std::stop_token const) -> async::Task<uimodel::LibraryScanOutcome>
      { co_return uimodel::LibraryScanOutcome{.verdict = uimodel::LibraryScanVerdict::Failed}; });

    controller.start();
    controller.cancel();
    REQUIRE(fixture.executor->drainUntil([&] { return controller.phase() == LibraryScanController::Phase::Idle; }));

    CHECK(lastMessage(fixture.runtimePtr->notifications()) ==
          uimodel::formatLibraryScanMessage(
            ao::test::englishMessageCatalog(), {.verdict = uimodel::LibraryScanVerdict::Failed}));
  }

  TEST_CASE("LibraryScanController - retire suppresses late presentation", "[tui][unit][scan][concurrency]")
  {
    auto fixture = ScanFixture{};
    auto* const runtime = &fixture.runtimePtr->async();
    auto controller = fixture.makeController(
      [runtime](std::stop_token const stopToken)
      { return waitThenOutcome(runtime, {.verdict = uimodel::LibraryScanVerdict::Complete}, stopToken); });

    controller.start();
    REQUIRE(fixture.sleeperPtr->waitForCallCount(1));
    controller.retire();
    REQUIRE(fixture.sleeperPtr->waitForCancellation(0));
    fixture.executor->drain();

    CHECK(controller.phase() == LibraryScanController::Phase::Retired);
    CHECK(fixture.runtimePtr->notifications().feed().entries.empty());
  }

  TEST_CASE("LibraryScanController - cancel and start are no-ops after retire", "[tui][unit][scan]")
  {
    auto fixture = ScanFixture{};
    auto controller = fixture.makeController([](std::stop_token const) -> async::Task<uimodel::LibraryScanOutcome>
                                             { co_return uimodel::LibraryScanOutcome{}; });

    controller.retire();
    controller.start();
    controller.cancel();
    fixture.executor->drain();

    CHECK(controller.phase() == LibraryScanController::Phase::Retired);
    CHECK(fixture.runtimePtr->notifications().feed().entries.empty());
  }

  TEST_CASE("LibraryScanController - production scan applies eagerly and leaves no identity backfill",
            "[tui][unit][scan]")
  {
    auto fixture = ScanFixture{};
    std::filesystem::copy_file(
      audio::test::requireAudioFixture("basic_metadata.flac"), fixture.runtimePtr->musicRoot() / "song.flac");
    auto controller = LibraryScanController{fixture.runtimePtr->async(),
                                            fixture.runtimePtr->library().jobs(),
                                            fixture.runtimePtr->notifications(),
                                            ao::test::englishMessageCatalog()};

    controller.start();
    REQUIRE(fixture.executor->drainUntil([&] { return controller.phase() == LibraryScanController::Phase::Idle; }));
    CHECK(lastMessage(fixture.runtimePtr->notifications()) ==
          uimodel::formatLibraryScanMessage(
            ao::test::englishMessageCatalog(),
            {.verdict = uimodel::LibraryScanVerdict::Complete, .summary = {.newCount = 1}}));

    controller.start();
    REQUIRE(fixture.executor->drainUntil([&] { return controller.phase() == LibraryScanController::Phase::Idle; }));
    CHECK(lastMessage(fixture.runtimePtr->notifications()) ==
          uimodel::formatLibraryScanMessage(
            ao::test::englishMessageCatalog(), {.verdict = uimodel::LibraryScanVerdict::UpToDate}));

    auto const backfillRes =
      rt::test::runRuntimeTask(*fixture.runtimePtr, fixture.runtimePtr->library().jobs().backfillAudioIdentityAsync());
    REQUIRE(backfillRes);
    CHECK(backfillRes->completedCount == 0);
    CHECK(backfillRes->skippedCount == 0);
    CHECK(backfillRes->failureCount == 0);
  }
} // namespace ao::tui::test
