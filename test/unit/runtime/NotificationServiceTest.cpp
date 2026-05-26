// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/rt/CorePrimitives.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/StateTypes.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace ao::rt::test
{
  TEST_CASE("NotificationService - post publishes NotificationPosted", "[app][runtime][notification]")
  {
    auto service = NotificationService{};

    auto receivedId = kInvalidNotificationId;
    auto sub = service.onPosted([&](auto id) { receivedId = id; });

    auto id = service.post(NotificationSeverity::Info, "test message");
    CHECK(receivedId == id);
  }

  TEST_CASE("NotificationService - dismiss publishes NotificationDismissed", "[app][runtime][notification]")
  {
    auto service = NotificationService{};

    auto id = service.post(NotificationSeverity::Warning, "warning");

    auto dismissedId = kInvalidNotificationId;
    auto sub = service.onDismissed([&](auto id) { dismissedId = id; });

    service.dismiss(id);
    CHECK(dismissedId == id);
  }

  TEST_CASE("NotificationService - dismiss non-existent does not publish", "[app][runtime][notification]")
  {
    auto service = NotificationService{};

    bool published = false;
    auto sub = service.onDismissed([&](auto const&) { published = true; });

    service.dismiss(NotificationId{999});
    CHECK_FALSE(published);
  }

  TEST_CASE("NotificationService - multiple posts assign distinct IDs", "[app][runtime][notification]")
  {
    auto service = NotificationService{};

    auto id1 = service.post(NotificationSeverity::Info, "first");
    auto id2 = service.post(NotificationSeverity::Info, "second");
    CHECK(id1 != id2);
  }

  TEST_CASE("NotificationService - rich post stores content state", "[app][runtime][notification]")
  {
    auto service = NotificationService{};

    auto const id = service.post(NotificationRequest{
      .severity = NotificationSeverity::Info,
      .message = "Importing library",
      .sticky = true,
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

  TEST_CASE("NotificationService - updateProgress publishes NotificationUpdated", "[app][runtime][notification]")
  {
    auto service = NotificationService{};

    auto const id = service.post(NotificationSeverity::Info, "Scanning", true);

    auto updatedId = kInvalidNotificationId;
    auto sub = service.onUpdated([&](auto incomingId) { updatedId = incomingId; });

    service.updateProgress(id,
                           NotificationProgressState{
                             .mode = NotificationProgressMode::Fraction,
                             .fraction = 0.5,
                             .label = "Halfway",
                           });

    auto const feed = service.feed();
    REQUIRE(feed.entries.size() == 1);

    auto const& entry = feed.entries.front();
    CHECK(updatedId == id);
    REQUIRE(entry.content.optProgress);
    CHECK(entry.content.optProgress->fraction == Catch::Approx{0.5});
    CHECK(entry.content.optProgress->label == "Halfway");
  }

  TEST_CASE("NotificationService - clearProgress publishes only when progress exists", "[app][runtime][notification]")
  {
    auto service = NotificationService{};

    auto const id = service.post(NotificationSeverity::Info, "Scanning", true);

    std::int32_t updatedCount = 0;
    auto sub = service.onUpdated([&](auto const&) { ++updatedCount; });

    service.clearProgress(id);
    CHECK(updatedCount == 0);

    service.updateProgress(id, NotificationProgressState{});
    service.clearProgress(id);

    auto const feed = service.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK_FALSE(feed.entries.front().content.optProgress);
    CHECK(updatedCount == 2);
  }

  TEST_CASE("NotificationService - dismissAll does not emit per-item events", "[app][runtime][notification]")
  {
    auto service = NotificationService{};

    service.post(NotificationSeverity::Info, "a");
    service.post(NotificationSeverity::Info, "b");

    std::int32_t dismissedCount = 0;
    auto sub = service.onDismissed([&](auto const&) { ++dismissedCount; });

    service.dismissAll();
    CHECK(dismissedCount == 0);
  }

  TEST_CASE("NotificationService - dismissAll publishes feed change", "[app][runtime][notification]")
  {
    auto service = NotificationService{};

    service.post(NotificationSeverity::Info, "a");
    service.post(NotificationSeverity::Info, "b");

    std::int32_t changedCount = 0;
    auto sub = service.onChanged([&] { ++changedCount; });

    service.dismissAll();
    service.dismissAll();

    CHECK(changedCount == 1);
  }

  TEST_CASE("NotificationService - feed revision changes on mutations", "[app][runtime][notification]")
  {
    auto service = NotificationService{};

    auto const initialRevision = service.feed().revision;
    auto const id = service.post(NotificationSeverity::Info, "a");
    auto const postedRevision = service.feed().revision;

    service.updateProgress(id, NotificationProgressState{});
    auto const updatedRevision = service.feed().revision;

    service.dismiss(id);

    CHECK(postedRevision > initialRevision);
    CHECK(updatedRevision > postedRevision);
    CHECK(service.feed().revision > updatedRevision);
  }

  TEST_CASE("NotificationService - updateContent modifies content and emits signals", "[app][runtime][notification]")
  {
    auto service = NotificationService{};
    auto id = service.post(NotificationSeverity::Info, "old");

    bool updatedFired = false;
    auto sub1 = service.onUpdated(
      [&](auto updatedId)
      {
        if (updatedId == id)
        {
          updatedFired = true;
        }
      });

    bool changedFired = false;
    auto sub2 = service.onChanged([&] { changedFired = true; });

    service.updateContent(id, NotificationContentState{.title = "new"});

    CHECK(updatedFired);
    CHECK(changedFired);

    auto feed = service.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries[0].content.title == "new");
  }
}
