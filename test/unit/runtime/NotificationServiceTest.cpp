// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include <ao/Exception.h>
#include <ao/async/AsyncExceptionHandler.h>
#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    struct NotificationServiceFixture final
    {
      explicit NotificationServiceFixture(async::AsyncExceptionHandler exceptionHandler = {},
                                          NotificationFeedLimits limits = {})
        : runtime{executor, 1, std::move(exceptionHandler)}, service{runtime, limits}
      {
      }

      InlineExecutor executor;
      async::Runtime runtime;
      NotificationService service;
    };
  } // namespace

  TEST_CASE("NotificationService - post publishes one revision-correlated snapshot", "[runtime][unit][notification]")
  {
    auto fixture = NotificationServiceFixture{};
    auto& service = fixture.service;
    auto updates = std::vector<NotificationFeedUpdate>{};
    auto sub = service.onFeedUpdated([&](NotificationFeedUpdate const& update) { updates.push_back(update); });

    auto const reply = service.post(NotificationSeverity::Info, "test message", NotificationLifetime::sessionHistory());
    auto const id = reply.id;

    CHECK(reply.outcome == NotificationMutationOutcome::Applied);
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
    auto fixture = NotificationServiceFixture{};
    auto& service = fixture.service;

    auto const firstId = service.post(NotificationSeverity::Info, "first", NotificationLifetime::sessionHistory()).id;
    auto const secondId = service.post(NotificationSeverity::Info, "second", NotificationLifetime::sessionHistory()).id;

    CHECK(firstId != secondId);
    CHECK(firstId == NotificationId{1});
    CHECK(secondId == NotificationId{2});
  }

  TEST_CASE("NotificationService - rich post stores content state", "[runtime][unit][notification]")
  {
    auto fixture = NotificationServiceFixture{};
    auto& service = fixture.service;

    auto const reply = service.post(NotificationRequest{
      .severity = NotificationSeverity::Info,
      .message = "Importing library",
      .lifetime = NotificationLifetime::untilDismissed(),
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
    auto const id = reply.id;

    REQUIRE(reply.outcome == NotificationMutationOutcome::Applied);
    auto const feed = service.feed();
    REQUIRE(feed.entries.size() == 1);

    auto const& entry = feed.entries.front();
    CHECK(entry.id == id);
    CHECK(entry.message == "Importing library");
    CHECK(entry.lifetime == NotificationLifetime::untilDismissed());
    CHECK(entry.lifetimeGeneration == 0);
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
    auto fixture = NotificationServiceFixture{};
    auto& service = fixture.service;
    auto const id = service.post(NotificationSeverity::Info, "old message", NotificationLifetime::sessionHistory()).id;
    auto updates = std::vector<NotificationFeedUpdate>{};
    auto sub = service.onFeedUpdated([&](NotificationFeedUpdate const& update) { updates.push_back(update); });

    CHECK(service.updateMessage(id, "new message").outcome == NotificationMutationOutcome::Applied);
    CHECK(service.updateContent(id, NotificationContentState{.title = "new title"}).outcome ==
          NotificationMutationOutcome::Applied);
    CHECK(service
            .updateProgress(id,
                            NotificationProgressState{
                              .mode = NotificationProgressMode::Fraction,
                              .fraction = 0.5,
                              .label = "Halfway",
                            })
            .outcome == NotificationMutationOutcome::Applied);
    CHECK(service.clearProgress(id).outcome == NotificationMutationOutcome::Applied);

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
    auto fixture = NotificationServiceFixture{};
    auto& service = fixture.service;
    auto const id = service.post(NotificationSeverity::Info, "Scanning", NotificationLifetime::sessionHistory()).id;
    std::int32_t updateCount = 0;
    auto sub = service.onFeedUpdated([&](NotificationFeedUpdate const&) { ++updateCount; });

    CHECK(service.updateMessage(NotificationId{999}, "missing").outcome == NotificationMutationOutcome::Missing);
    CHECK(service.updateMessage(id, "Scanning").outcome == NotificationMutationOutcome::Unchanged);
    CHECK(service.updateContent(NotificationId{999}, {}).outcome == NotificationMutationOutcome::Missing);
    CHECK(service.updateProgress(NotificationId{999}, {}).outcome == NotificationMutationOutcome::Missing);
    CHECK(service.clearProgress(id).outcome == NotificationMutationOutcome::Unchanged);
    CHECK(service.dismiss(NotificationId{999}).outcome == NotificationMutationOutcome::Missing);

    CHECK(updateCount == 0);
    CHECK(service.feed().revision == 1);
  }

  TEST_CASE("NotificationService - dismissal updates identify every removed entry", "[runtime][unit][notification]")
  {
    auto fixture = NotificationServiceFixture{};
    auto& service = fixture.service;
    auto const firstId = service.post(NotificationSeverity::Info, "a", NotificationLifetime::sessionHistory()).id;
    auto const secondId = service.post(NotificationSeverity::Info, "b", NotificationLifetime::sessionHistory()).id;
    auto const thirdId = service.post(NotificationSeverity::Info, "c", NotificationLifetime::sessionHistory()).id;
    auto updates = std::vector<NotificationFeedUpdate>{};
    auto sub = service.onFeedUpdated([&](NotificationFeedUpdate const& update) { updates.push_back(update); });

    CHECK(service.dismiss(secondId).outcome == NotificationMutationOutcome::Applied);
    CHECK(service.dismissAll().outcome == NotificationMutationOutcome::Applied);
    CHECK(service.dismissAll().outcome == NotificationMutationOutcome::Unchanged);

    REQUIRE(updates.size() == 2);
    CHECK(updates[0].mutationKind == NotificationFeedMutationKind::Dismissed);
    CHECK(updates[0].affectedIds == std::vector{secondId});
    CHECK(updates[1].mutationKind == NotificationFeedMutationKind::Cleared);
    CHECK(updates[1].affectedIds == std::vector{firstId, thirdId});
    REQUIRE(updates[1].feedPtr);
    CHECK(updates[1].feedPtr->entries.empty());
    CHECK(service.feed().revision == 5);
  }

  TEST_CASE("NotificationService - invalid bounded input is rejected without consuming identity",
            "[runtime][unit][notification]")
  {
    auto limits = NotificationFeedLimits{};
    limits.maxActionsPerEntry = 1;
    limits.maxTextBytes = 32;
    auto fixture = NotificationServiceFixture{{}, limits};
    auto& service = fixture.service;
    std::int32_t updateCount = 0;
    auto sub = service.onFeedUpdated([&](NotificationFeedUpdate const&) { ++updateCount; });

    CHECK(
      service.post(NotificationSeverity::Info, std::string(33, 'x'), NotificationLifetime::sessionHistory()).outcome ==
      NotificationMutationOutcome::Rejected);
    CHECK(service
            .post(NotificationRequest{
              .message = "too many actions",
              .lifetime = NotificationLifetime::sessionHistory(),
              .content =
                NotificationContentState{
                  .actions = {{.id = "first", .label = "First"}, {.id = "second", .label = "Second"}},
                },
            })
            .outcome == NotificationMutationOutcome::Rejected);
    CHECK(service
            .post(NotificationSeverity::Info,
                  "invalid duration",
                  NotificationLifetime::transient(std::chrono::milliseconds::zero()))
            .outcome == NotificationMutationOutcome::Rejected);
    CHECK(service
            .createOrUpdate(NotificationReportKey{},
                            NotificationRequest{
                              .message = "empty key",
                              .lifetime = NotificationLifetime::sessionHistory(),
                            })
            .outcome == NotificationMutationOutcome::Rejected);

    CHECK(service.feed().entries.empty());
    CHECK(service.feed().revision == 0);
    CHECK(updateCount == 0);

    auto const accepted = service.post(NotificationSeverity::Info, "accepted", NotificationLifetime::sessionHistory());
    CHECK(accepted.outcome == NotificationMutationOutcome::Applied);
    CHECK(accepted.id == NotificationId{1});
    CHECK(service.updateMessage(accepted.id, std::string(33, 'x')).outcome == NotificationMutationOutcome::Rejected);
    CHECK(service.feed().revision == 1);
    CHECK(service.feed().entries.front().message == "accepted");
  }

  TEST_CASE("NotificationService - history eviction is atomic and pinned exhaustion rejects",
            "[runtime][unit][notification]")
  {
    auto limits = NotificationFeedLimits{};
    limits.maxEntries = 3;
    limits.maxSessionHistoryEntries = 2;
    auto fixture = NotificationServiceFixture{{}, limits};
    auto& service = fixture.service;
    auto updates = std::vector<NotificationFeedUpdate>{};
    auto sub = service.onFeedUpdated([&](NotificationFeedUpdate const& update) { updates.push_back(update); });

    auto const firstId = service.post(NotificationSeverity::Info, "first", NotificationLifetime::sessionHistory()).id;
    auto const secondId = service.post(NotificationSeverity::Info, "second", NotificationLifetime::sessionHistory()).id;
    auto const thirdId = service.post(NotificationSeverity::Info, "third", NotificationLifetime::sessionHistory()).id;

    REQUIRE(updates.size() == 3);
    CHECK(updates.back().affectedIds == std::vector{thirdId});
    CHECK(updates.back().evictedIds == std::vector{firstId});
    CHECK(service.feed().entries[0].id == secondId);
    CHECK(service.feed().entries[1].id == thirdId);

    auto const firstPinned =
      service.post(NotificationSeverity::Warning, "pinned one", NotificationLifetime::untilDismissed());
    auto const secondPinned =
      service.post(NotificationSeverity::Warning, "pinned two", NotificationLifetime::untilDismissed());
    auto const thirdPinned =
      service.post(NotificationSeverity::Warning, "pinned three", NotificationLifetime::untilDismissed());

    REQUIRE(firstPinned.outcome == NotificationMutationOutcome::Applied);
    REQUIRE(secondPinned.outcome == NotificationMutationOutcome::Applied);
    REQUIRE(thirdPinned.outcome == NotificationMutationOutcome::Applied);
    CHECK(updates[4].evictedIds == std::vector{secondId});
    CHECK(updates[5].evictedIds == std::vector{thirdId});

    auto const rejected = service.post(NotificationSeverity::Error, "no room", NotificationLifetime::untilDismissed());
    CHECK(rejected.outcome == NotificationMutationOutcome::Rejected);
    CHECK(rejected.id == kInvalidNotificationId);
    CHECK(service.feed().revision == 6);
    REQUIRE(service.feed().entries.size() == 3);
    CHECK(service.feed().entries[0].id == firstPinned.id);
    CHECK(service.feed().entries[1].id == secondPinned.id);
    CHECK(service.feed().entries[2].id == thirdPinned.id);

    REQUIRE(service.dismiss(firstPinned.id).outcome == NotificationMutationOutcome::Applied);
    auto const acceptedAfterDismissal =
      service.post(NotificationSeverity::Error, "now fits", NotificationLifetime::untilDismissed());
    CHECK(acceptedAfterDismissal.outcome == NotificationMutationOutcome::Applied);
    CHECK(acceptedAfterDismissal.id == NotificationId{7});
  }

  TEST_CASE("NotificationService - total text bound evicts history before rejecting retained entries",
            "[runtime][unit][notification]")
  {
    auto limits = NotificationFeedLimits{};
    limits.maxEntries = 10;
    limits.maxSessionHistoryEntries = 10;
    limits.maxTotalTextBytes = 10;
    auto fixture = NotificationServiceFixture{{}, limits};
    auto& service = fixture.service;
    auto updates = std::vector<NotificationFeedUpdate>{};
    auto sub = service.onFeedUpdated([&](NotificationFeedUpdate const& update) { updates.push_back(update); });

    auto const history = service.post(NotificationRequest{
      .message = "12345",
      .lifetime = NotificationLifetime::sessionHistory(),
      .content = NotificationContentState{.templateId = ""},
    });
    auto const pinned = service.post(NotificationRequest{
      .message = "123456",
      .lifetime = NotificationLifetime::untilDismissed(),
      .content = NotificationContentState{.templateId = ""},
    });

    REQUIRE(history.outcome == NotificationMutationOutcome::Applied);
    REQUIRE(pinned.outcome == NotificationMutationOutcome::Applied);
    CHECK(updates.back().evictedIds == std::vector{history.id});
    REQUIRE(service.feed().entries.size() == 1);
    CHECK(service.feed().entries.front().id == pinned.id);

    auto const rejected = service.post(NotificationRequest{
      .message = "12345",
      .lifetime = NotificationLifetime::untilDismissed(),
      .content = NotificationContentState{.templateId = ""},
    });
    CHECK(rejected.outcome == NotificationMutationOutcome::Rejected);
    CHECK(service.feed().revision == 2);
    REQUIRE(service.feed().entries.size() == 1);
    CHECK(service.feed().entries.front().id == pinned.id);
  }

  TEST_CASE("NotificationService - keyed create-or-update owns correlation and suppresses identical updates",
            "[runtime][unit][notification]")
  {
    auto fixture = NotificationServiceFixture{};
    auto& service = fixture.service;
    auto updates = std::vector<NotificationFeedUpdate>{};
    auto sub = service.onFeedUpdated([&](NotificationFeedUpdate const& update) { updates.push_back(update); });
    auto const key = NotificationReportKey{"library.scan.current"};
    auto request = NotificationRequest{
      .severity = NotificationSeverity::Warning,
      .message = "Skipped 1 file",
      .lifetime = NotificationLifetime::sessionHistory(),
    };

    auto const created = service.createOrUpdate(key, request);
    auto const unchanged = service.createOrUpdate(key, request);
    request.message = "Skipped 2 files";
    auto const updated = service.createOrUpdate(key, request);

    REQUIRE(created.outcome == NotificationMutationOutcome::Applied);
    CHECK(unchanged ==
          (NotificationMutationReply{.outcome = NotificationMutationOutcome::Unchanged, .id = created.id}));
    CHECK(updated == (NotificationMutationReply{.outcome = NotificationMutationOutcome::Applied, .id = created.id}));
    REQUIRE(updates.size() == 2);
    CHECK(updates[0].mutationKind == NotificationFeedMutationKind::Posted);
    CHECK(updates[1].mutationKind == NotificationFeedMutationKind::ReportUpdated);
    CHECK(updates[1].affectedIds == std::vector{created.id});
    CHECK(service.feed().revision == 2);
    REQUIRE(service.feed().entries.size() == 1);
    REQUIRE(service.feed().entries.front().optReportKey);
    CHECK(*service.feed().entries.front().optReportKey == key);
    CHECK(service.feed().entries.front().message == "Skipped 2 files");

    REQUIRE(service.dismiss(created.id).outcome == NotificationMutationOutcome::Applied);
    auto const recreated = service.createOrUpdate(key, request);
    CHECK(recreated.outcome == NotificationMutationOutcome::Applied);
    CHECK(recreated.id != created.id);
  }

  TEST_CASE("NotificationService - observer failure is contained across committed revisions",
            "[runtime][regression][notification][concurrency]")
  {
    auto recorder = AsyncExceptionRecorder{};
    auto fixture = NotificationServiceFixture{recorder.handler()};
    auto& service = fixture.service;
    auto laterObserverRevisions = std::vector<std::uint64_t>{};
    auto throwingSub =
      service.onFeedUpdated([](NotificationFeedUpdate const&) { throwException<Exception>("observer failed"); });
    auto laterSub = service.onFeedUpdated([&](NotificationFeedUpdate const& update)
                                          { laterObserverRevisions.push_back(update.revision); });

    CHECK_NOTHROW(service.post(NotificationSeverity::Warning, "committed", NotificationLifetime::sessionHistory()));
    CHECK_NOTHROW(service.post(NotificationSeverity::Info, "later", NotificationLifetime::sessionHistory()));

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
    auto fixture = NotificationServiceFixture{};
    auto& service = fixture.service;
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

          CHECK(service.updateMessage(update.affectedIds.front(), "updated").outcome ==
                NotificationMutationOutcome::Applied);
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

    service.post(NotificationSeverity::Info, "initial", NotificationLifetime::sessionHistory());

    CHECK(observedRevisions == std::vector<std::uint64_t>{1, 2});
    CHECK(observedMessages == std::vector<std::string>{"initial", "updated"});
    CHECK(service.feed().entries.front().message == "updated");
  }

  TEST_CASE("NotificationService - reentrant post advances the id watermark before publication",
            "[runtime][regression][notification][concurrency]")
  {
    auto fixture = NotificationServiceFixture{};
    auto& service = fixture.service;
    auto nestedId = kInvalidNotificationId;
    auto sub = service.onFeedUpdated(
      [&](NotificationFeedUpdate const& update)
      {
        if (update.revision == 1)
        {
          nestedId = service.post(NotificationSeverity::Info, "nested", NotificationLifetime::sessionHistory()).id;
        }
      });

    auto const outerId = service.post(NotificationSeverity::Info, "outer", NotificationLifetime::sessionHistory()).id;

    CHECK(outerId == NotificationId{1});
    CHECK(nestedId == NotificationId{2});
    REQUIRE(service.feed().entries.size() == 2);
    CHECK(service.feed().entries[0].id == outerId);
    CHECK(service.feed().entries[1].id == nestedId);
  }
} // namespace ao::rt::test
