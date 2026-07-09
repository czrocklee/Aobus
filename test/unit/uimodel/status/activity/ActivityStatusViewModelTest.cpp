// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryTaskService.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/uimodel/status/activity/ActivityStatusViewModel.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("ActivityStatusViewModel - projects runtime feed updates", "[uimodel][unit][status][activity]")
  {
    auto notifications = rt::NotificationService{};
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
      auto const id = notifications.post(rt::NotificationSeverity::Warning, "Partial import", true);

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

    SECTION("transient info notifications expire through the injected clock")
    {
      notifications.post(rt::NotificationRequest{.severity = rt::NotificationSeverity::Info,
                                                 .message = "Saved playlist",
                                                 .optTimeout = std::chrono::milliseconds{1500}});

      CHECK(latest.compact.kind == ActivityStatusKind::Info);
      CHECK(latest.compact.text == "Saved playlist");
      CHECK(viewModel.hasPendingAutoDismiss());

      now += std::chrono::milliseconds{1499};
      CHECK_FALSE(viewModel.expireTransientIfDue());
      CHECK(latest.compact.kind == ActivityStatusKind::Info);

      now += std::chrono::milliseconds{1};
      CHECK(viewModel.expireTransientIfDue());
      CHECK(latest.compact.kind == ActivityStatusKind::Idle);
      CHECK_FALSE(viewModel.hasPendingAutoDismiss());
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

      viewModel.handleLibraryTaskCompleted(4);

      CHECK(latest.compact.kind == ActivityStatusKind::Success);
      CHECK(latest.compact.text == "Scan complete: 4 tracks added");
      CHECK_FALSE(latest.compact.optProgressFraction);
    }

    CHECK(renderCount > 0);
  }

  TEST_CASE("ActivityStatusViewModel - projects library task events from LibraryChanges",
            "[uimodel][regression][status][activity]")
  {
    auto notifications = rt::NotificationService{};
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
    auto executor = rt::test::MockExecutor{};
    auto runtime = async::Runtime{executor};
    auto taskService = rt::LibraryTaskService{runtime, libraryFixture.library(), changes};
    auto applyTask = [](rt::LibraryTaskService* service) -> async::Task<Result<rt::ScanApplyResult>>
    {
      auto plan = rt::ScanPlan{};
      plan.items.push_back(rt::ScanItem{.uri = "file:///fake/first.flac",
                                        .oldUri = {},
                                        .fullPath = "/fake/first.flac",
                                        .classification = rt::ScanClassification::New});
      co_return co_await service->applyScanPlanAsync(std::move(plan));
    };

    auto const result = runtime.spawn(applyTask(&taskService)).get();

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
    CHECK(latest.compact.kind == ActivityStatusKind::Success);
    CHECK(latest.compact.text == "Library is up to date");
  }
} // namespace ao::uimodel::test
