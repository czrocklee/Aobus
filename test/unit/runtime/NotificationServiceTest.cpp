// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include <ao/Exception.h>
#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("NotificationService - post publishes one revision-correlated snapshot", "[runtime][unit][notification]")
  {
    auto executor = InlineExecutor{};
    auto service = NotificationService{executor};
    auto updates = std::vector<NotificationFeedUpdate>{};
    auto sub = service.onFeedUpdated([&](NotificationFeedUpdate const& update) { updates.push_back(update); });

    auto const id = service.post(NotificationSeverity::Info, "test message");

    REQUIRE(updates.size() == 1);
    auto const& update = updates.front();
    CHECK(update.revision == 1);
    CHECK(update.mutationKind == NotificationFeedMutationKind::Posted);
    REQUIRE(update.affectedIds.size() == 1);
    CHECK(update.affectedIds.front() == id);
    REQUIRE(update.feedPtr);
    CHECK(update.feedPtr->revision == update.revision);
    REQUIRE(update.feedPtr->entries.size() == 1);
    CHECK(update.feedPtr->entries.front().id == id);
    CHECK(update.feedPtr->entries.front().message == "test message");
  }

  TEST_CASE("NotificationService - multiple posts assign distinct ids", "[runtime][unit][notification]")
  {
    auto executor = InlineExecutor{};
    auto service = NotificationService{executor};

    auto const firstId = service.post(NotificationSeverity::Info, "first");
    auto const secondId = service.post(NotificationSeverity::Info, "second");

    CHECK(firstId != secondId);
    CHECK(firstId == NotificationId{1});
    CHECK(secondId == NotificationId{2});
  }

  TEST_CASE("NotificationService - rich post stores content state", "[runtime][unit][notification]")
  {
    auto executor = InlineExecutor{};
    auto service = NotificationService{executor};

    auto const id = service.post(NotificationRequest{
      .severity = NotificationSeverity::Info,
      .message = "Importing library",
      .sticky = true,
      .activityPresentation = NotificationActivityPresentation::DetailOnly,
      .content =
        NotificationContentState{
          .templateId = "notification.import-progress",
          .title = "Library import",
          .iconName = "document-open-symbolic",
          .actions = {{.id = "cancel", .label = "Cancel"}},
          .optProgress =
            NotificationProgressState{
              .mode = NotificationProgressMode::Fraction,
              .fraction = 0.25,
              .label = "25%",
            },
        },
    });

    auto const feed = service.feed();
    REQUIRE(feed.entries.size() == 1);

    auto const& entry = feed.entries.front();
    CHECK(entry.id == id);
    CHECK(entry.message == "Importing library");
    CHECK(entry.sticky);
    CHECK(entry.activityPresentation == NotificationActivityPresentation::DetailOnly);
    CHECK(entry.content.templateId == "notification.import-progress");
    CHECK(entry.content.title == "Library import");
    CHECK(entry.content.iconName == "document-open-symbolic");
    REQUIRE(entry.content.actions.size() == 1);
    CHECK(entry.content.actions.front().id == "cancel");
    REQUIRE(entry.content.optProgress);
    CHECK(entry.content.optProgress->mode == NotificationProgressMode::Fraction);
    CHECK(entry.content.optProgress->fraction == Catch::Approx{0.25});
    CHECK(entry.content.optProgress->label == "25%");
  }

  TEST_CASE("NotificationService - update commands publish their exact mutation kinds", "[runtime][unit][notification]")
  {
    auto executor = InlineExecutor{};
    auto service = NotificationService{executor};
    auto const id = service.post(NotificationSeverity::Info, "old message", true);
    auto updates = std::vector<NotificationFeedUpdate>{};
    auto sub = service.onFeedUpdated([&](NotificationFeedUpdate const& update) { updates.push_back(update); });

    CHECK(service.updateMessage(id, "new message"));
    service.updateContent(id, NotificationContentState{.title = "new title"});
    service.updateProgress(id,
                           NotificationProgressState{
                             .mode = NotificationProgressMode::Fraction,
                             .fraction = 0.5,
                             .label = "Halfway",
                           });
    service.clearProgress(id);

    REQUIRE(updates.size() == 4);
    CHECK(updates[0].mutationKind == NotificationFeedMutationKind::MessageUpdated);
    CHECK(updates[1].mutationKind == NotificationFeedMutationKind::ContentUpdated);
    CHECK(updates[2].mutationKind == NotificationFeedMutationKind::ProgressUpdated);
    CHECK(updates[3].mutationKind == NotificationFeedMutationKind::ProgressCleared);

    for (std::size_t index = 0; index < updates.size(); ++index)
    {
      CHECK(updates[index].revision == index + 2);
      REQUIRE(updates[index].affectedIds.size() == 1);
      CHECK(updates[index].affectedIds.front() == id);
      REQUIRE(updates[index].feedPtr);
      CHECK(updates[index].feedPtr->revision == updates[index].revision);
    }

    auto const feed = service.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().message == "new message");
    CHECK(feed.entries.front().content.title == "new title");
    CHECK_FALSE(feed.entries.front().content.optProgress);
  }

  TEST_CASE("NotificationService - ineffective commands do not publish", "[runtime][unit][notification]")
  {
    auto executor = InlineExecutor{};
    auto service = NotificationService{executor};
    auto const id = service.post(NotificationSeverity::Info, "Scanning", true);
    std::int32_t updateCount = 0;
    auto sub = service.onFeedUpdated([&](NotificationFeedUpdate const&) { ++updateCount; });

    CHECK_FALSE(service.updateMessage(NotificationId{999}, "missing"));
    service.updateContent(NotificationId{999}, {});
    service.updateProgress(NotificationId{999}, {});
    service.clearProgress(id);
    service.dismiss(NotificationId{999});

    CHECK(updateCount == 0);
    CHECK(service.feed().revision == 1);
  }

  TEST_CASE("NotificationService - dismissal updates identify every removed entry", "[runtime][unit][notification]")
  {
    auto executor = InlineExecutor{};
    auto service = NotificationService{executor};
    auto const firstId = service.post(NotificationSeverity::Info, "a");
    auto const secondId = service.post(NotificationSeverity::Info, "b");
    auto const thirdId = service.post(NotificationSeverity::Info, "c");
    auto updates = std::vector<NotificationFeedUpdate>{};
    auto sub = service.onFeedUpdated([&](NotificationFeedUpdate const& update) { updates.push_back(update); });

    service.dismiss(secondId);
    service.dismissAll();
    service.dismissAll();

    REQUIRE(updates.size() == 2);
    CHECK(updates[0].mutationKind == NotificationFeedMutationKind::Dismissed);
    CHECK(updates[0].affectedIds == std::vector{secondId});
    CHECK(updates[1].mutationKind == NotificationFeedMutationKind::Cleared);
    CHECK(updates[1].affectedIds == std::vector{firstId, thirdId});
    REQUIRE(updates[1].feedPtr);
    CHECK(updates[1].feedPtr->entries.empty());
    CHECK(service.feed().revision == 5);
  }

  TEST_CASE("NotificationService - observer failure is contained across committed revisions",
            "[runtime][regression][notification][concurrency]")
  {
    auto executor = InlineExecutor{};
    auto recorder = AsyncExceptionRecorder{};
    auto service = NotificationService{executor, recorder.handler()};
    auto laterObserverRevisions = std::vector<std::uint64_t>{};
    auto throwingSub =
      service.onFeedUpdated([](NotificationFeedUpdate const&) { throwException<Exception>("observer failed"); });
    auto laterSub = service.onFeedUpdated([&](NotificationFeedUpdate const& update)
                                          { laterObserverRevisions.push_back(update.revision); });

    CHECK_NOTHROW(service.post(NotificationSeverity::Warning, "committed"));
    CHECK_NOTHROW(service.post(NotificationSeverity::Info, "later"));

    CHECK(service.feed().revision == 2);
    CHECK(laterObserverRevisions == std::vector<std::uint64_t>{1, 2});

    auto const exceptions = recorder.snapshot();
    REQUIRE(exceptions.size() == 2);
    checkRecordedException<Exception>(exceptions[0], "notification feed observer at revision 1");
    checkRecordedException<Exception>(exceptions[1], "notification feed observer at revision 2");
  }

  TEST_CASE("NotificationService - reentrant mutation preserves immutable revision order",
            "[runtime][regression][notification][concurrency]")
  {
    auto executor = InlineExecutor{};
    auto service = NotificationService{executor};
    auto observedRevisions = std::vector<std::uint64_t>{};
    auto observedMessages = std::vector<std::string>{};
    auto mutatingSub = service.onFeedUpdated(
      [&](NotificationFeedUpdate const& update)
      {
        if (update.mutationKind == NotificationFeedMutationKind::Posted)
        {
          CHECK(update.affectedIds.size() == 1);

          if (update.affectedIds.size() != 1)
          {
            return;
          }

          CHECK(service.updateMessage(update.affectedIds.front(), "updated"));
        }
      });
    auto observingSub = service.onFeedUpdated(
      [&](NotificationFeedUpdate const& update)
      {
        observedRevisions.push_back(update.revision);
        CHECK(update.feedPtr);

        if (!update.feedPtr)
        {
          return;
        }

        CHECK(update.feedPtr->entries.size() == 1);

        if (update.feedPtr->entries.size() != 1)
        {
          return;
        }

        observedMessages.push_back(update.feedPtr->entries.front().message);
      });

    service.post(NotificationSeverity::Info, "initial");

    CHECK(observedRevisions == std::vector<std::uint64_t>{1, 2});
    CHECK(observedMessages == std::vector<std::string>{"initial", "updated"});
    CHECK(service.feed().entries.front().message == "updated");
  }

  TEST_CASE("NotificationService - reentrant post advances the id watermark before publication",
            "[runtime][regression][notification][concurrency]")
  {
    auto executor = InlineExecutor{};
    auto service = NotificationService{executor};
    auto nestedId = kInvalidNotificationId;
    auto sub = service.onFeedUpdated(
      [&](NotificationFeedUpdate const& update)
      {
        if (update.revision == 1)
        {
          nestedId = service.post(NotificationSeverity::Info, "nested");
        }
      });

    auto const outerId = service.post(NotificationSeverity::Info, "outer");

    CHECK(outerId == NotificationId{1});
    CHECK(nestedId == NotificationId{2});
    REQUIRE(service.feed().entries.size() == 2);
    CHECK(service.feed().entries[0].id == outerId);
    CHECK(service.feed().entries[1].id == nestedId);
  }
} // namespace ao::rt::test
