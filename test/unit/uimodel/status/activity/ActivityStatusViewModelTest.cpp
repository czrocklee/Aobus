// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/LibraryTaskService.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/uimodel/status/activity/ActivityStatusViewModel.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("ActivityStatusViewModel - projects runtime feed updates", "[uimodel][unit][status][activity]")
  {
    auto executor = rt::test::InlineExecutor{};
    auto sleeper = rt::test::ControlledSleeper{};
    auto asyncRuntime = async::Runtime{executor, 1, {}, &sleeper};
    auto notifications = rt::NotificationService{asyncRuntime};
    auto now = std::chrono::steady_clock::time_point{};
    auto latest = ActivityStatusViewState{};
    std::int32_t renderCount = 0;
    auto viewModel = ActivityStatusViewModel{
      notifications,
      [&](ActivityStatusViewState const& view)
      {
        latest = view;
        ++renderCount;
      },
      ActivityStatusViewModelOptions{.clock = [&] { return now; }},
    };

    SECTION("persistent warnings render and compact dismiss keeps the feed")
    {
      auto const renderCountBeforePost = renderCount;
      auto const id =
        notifications
          .post(rt::NotificationSeverity::Warning, "Partial import", rt::NotificationLifetime::untilDismissed())
          .id;

      CHECK(renderCount == renderCountBeforePost + 1);
      CHECK(latest.compact.kind == ActivityStatusKind::Warning);
      CHECK(latest.compact.text == "Partial import");
      CHECK(latest.compact.dismissible);
      CHECK_FALSE(viewModel.hasPendingAutoDismiss());

      viewModel.dismissCompact();

      CHECK(latest.compact.kind == ActivityStatusKind::Idle);
      auto const feed = notifications.feed();
      REQUIRE(feed.entries.size() == 1);
      CHECK(feed.entries.front().id == id);
      REQUIRE(latest.detail.items.size() == 1);
      CHECK(latest.detail.items.front().message == "Partial import");
    }

    SECTION("retained info compact expires presentation-locally through the injected clock")
    {
      notifications.post(rt::NotificationSeverity::Info, "Saved playlist", rt::NotificationLifetime::sessionHistory());

      CHECK(latest.compact.kind == ActivityStatusKind::Info);
      CHECK(latest.compact.text == "Saved playlist");
      CHECK(viewModel.hasPendingAutoDismiss());

      now += kActivityStatusDefaultAutoDismissTimeout - std::chrono::milliseconds{1};
      CHECK_FALSE(viewModel.expireTransientIfDue());
      CHECK(latest.compact.kind == ActivityStatusKind::Info);

      now += std::chrono::milliseconds{1};
      CHECK(viewModel.expireTransientIfDue());
      CHECK(latest.compact.kind == ActivityStatusKind::Idle);
      CHECK_FALSE(viewModel.hasPendingAutoDismiss());
      CHECK(notifications.feed().entries.size() == 1);
    }

    SECTION("runtime-transient info does not create a presentation-local deadline")
    {
      notifications.post(rt::NotificationSeverity::Info, "Saved playlist", rt::NotificationLifetime::transient());

      CHECK(latest.compact.kind == ActivityStatusKind::Info);
      CHECK_FALSE(viewModel.hasPendingAutoDismiss());
    }

    SECTION("visible transient updates render the accepted revision once")
    {
      auto const id =
        notifications
          .post(rt::NotificationSeverity::Info, "Saving playlist", rt::NotificationLifetime::sessionHistory())
          .id;
      REQUIRE(latest.compact.kind == ActivityStatusKind::Info);
      REQUIRE(latest.compact.text == "Saving playlist");

      auto const renderCountBeforeUpdate = renderCount;
      CHECK(notifications.updateMessage(id, "Playlist saved").outcome == rt::NotificationMutationOutcome::Applied);

      CHECK(renderCount == renderCountBeforeUpdate + 1);
      CHECK(latest.compact.kind == ActivityStatusKind::Info);
      CHECK(latest.compact.text == "Playlist saved");
    }

    SECTION("library task runtime events reuse activity projection")
    {
      viewModel.handleLibraryTaskProgress("Updating: status-progress.flac", 0.625);

      CHECK(latest.compact.kind == ActivityStatusKind::Processing);
      CHECK(latest.compact.text == "Updating library");
      REQUIRE(latest.compact.optProgressFraction);
      CHECK(*latest.compact.optProgressFraction == 0.625);
      REQUIRE(latest.detail.optLibraryTask);
      CHECK(latest.detail.optLibraryTask->message == "Updating: status-progress.flac");

      viewModel.handleLibraryTaskCompleted(rt::LibraryChanges::LibraryTaskCompleted{.affectedCount = 4});

      CHECK(latest.compact.kind == ActivityStatusKind::Success);
      CHECK(latest.compact.text == "Scan complete: 4 tracks added");
      CHECK_FALSE(latest.compact.optProgressFraction);
    }

    CHECK(renderCount > 0);
  }

  TEST_CASE("ActivityStatusViewModel - accepted history eviction projects one bounded snapshot",
            "[uimodel][unit][status][activity]")
  {
    auto executor = rt::test::InlineExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto limits = rt::NotificationFeedLimits{};
    limits.maxEntries = 1;
    limits.maxSessionHistoryEntries = 1;
    auto notifications = rt::NotificationService{runtime, limits};
    auto latest = ActivityStatusViewState{};
    std::int32_t renderCount = 0;
    auto viewModel = ActivityStatusViewModel{
      notifications,
      [&](ActivityStatusViewState const& view)
      {
        latest = view;
        ++renderCount;
      },
    };
    auto const renderCountBeforePosts = renderCount;

    auto const first = notifications.post(
      rt::NotificationSeverity::Warning, "First warning", rt::NotificationLifetime::sessionHistory());
    auto const second = notifications.post(
      rt::NotificationSeverity::Warning, "Second warning", rt::NotificationLifetime::sessionHistory());

    REQUIRE(first.outcome == rt::NotificationMutationOutcome::Applied);
    REQUIRE(second.outcome == rt::NotificationMutationOutcome::Applied);
    CHECK(renderCount == renderCountBeforePosts + 2);
    REQUIRE(latest.detail.items.size() == 1);
    CHECK(latest.detail.items.front().id == second.id);
    CHECK(latest.detail.items.front().message == "Second warning");
    CHECK(latest.compact.kind == ActivityStatusKind::Warning);
    CHECK(latest.compact.text == "Second warning");
    REQUIRE(notifications.feed().entries.size() == 1);
    CHECK(notifications.feed().entries.front().id == second.id);
  }

  TEST_CASE("ActivityStatusViewModel - hidden post eviction removes the compact source",
            "[uimodel][regression][status][activity]")
  {
    auto executor = rt::test::InlineExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto limits = rt::NotificationFeedLimits{};
    limits.maxEntries = 1;
    limits.maxSessionHistoryEntries = 1;
    auto notifications = rt::NotificationService{runtime, limits};
    auto latest = ActivityStatusViewState{};
    auto viewModel =
      ActivityStatusViewModel{notifications, [&](ActivityStatusViewState const& view) { latest = view; }};

    auto const visible = notifications.post(
      rt::NotificationSeverity::Warning, "Visible warning", rt::NotificationLifetime::sessionHistory());
    REQUIRE(visible.outcome == rt::NotificationMutationOutcome::Applied);
    REQUIRE(latest.compact.kind == ActivityStatusKind::Warning);

    auto const hidden = notifications.post(rt::NotificationRequest{
      .message = "Hidden replacement",
      .lifetime = rt::NotificationLifetime::sessionHistory(),
      .activityPresentation = rt::NotificationActivityPresentation::Hidden,
    });

    REQUIRE(hidden.outcome == rt::NotificationMutationOutcome::Applied);
    CHECK(latest.compact.kind == ActivityStatusKind::Idle);
    CHECK(latest.compact.sourceNotificationIds.empty());
    CHECK(latest.detail.items.empty());
    REQUIRE(notifications.feed().entries.size() == 1);
    CHECK(notifications.feed().entries.front().id == hidden.id);
  }

  TEST_CASE("ActivityStatusViewModel - projects library task events from LibraryChanges",
            "[uimodel][regression][status][activity]")
  {
    auto executor = rt::test::InlineExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto notifications = rt::NotificationService{runtime};
    auto changes = rt::LibraryChanges{};
    auto latest = ActivityStatusViewState{};
    auto rendered = std::vector<ActivityStatusViewState>{};
    auto viewModel = ActivityStatusViewModel{
      notifications,
      [&](ActivityStatusViewState const& view)
      {
        latest = view;
        rendered.push_back(view);
      },
      ActivityStatusViewModelOptions{.libraryChanges = &changes},
    };

    auto libraryFixture = rt::test::MusicLibraryFixture{};
    auto runtimeLibrary = rt::Library{runtime, libraryFixture.library(), changes};
    auto& taskService = runtimeLibrary.taskService();
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const targetFile = libraryFixture.root() / "first.flac";
    std::filesystem::copy_file(sourceFile, targetFile);
    auto plan = rt::LibraryScan{libraryFixture.library()}.buildPlan().value();
    std::filesystem::remove(targetFile);

    auto const result = runtime.spawn(taskService.applyScanPlanAsync(std::move(plan))).get();

    REQUIRE(result);
    CHECK(result->failureCount == 1);

    auto const* progressView = static_cast<ActivityStatusViewState const*>(nullptr);

    for (auto const& view : rendered)
    {
      if (view.compact.kind == ActivityStatusKind::Processing)
      {
        progressView = &view;
        break;
      }
    }

    REQUIRE(progressView != nullptr);
    CHECK(progressView->compact.text == "Updating library");
    REQUIRE(progressView->detail.optLibraryTask);
    CHECK(progressView->detail.optLibraryTask->message == "Updating: first.flac");
    CHECK(latest.compact.kind == ActivityStatusKind::Idle);
    CHECK(latest.compact.text.empty());
  }
} // namespace ao::uimodel::test
