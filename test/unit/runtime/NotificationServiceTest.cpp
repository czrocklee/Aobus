// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include <runtime/NotificationService.h>

namespace ao::rt::test
{
  TEST_CASE("NotificationService - post publishes NotificationPosted", "[app][runtime][notification]")
  {
    auto service = NotificationService{};

    auto receivedId = NotificationId{};
    auto sub = service.onPosted([&](auto id) { receivedId = id; });

    auto id = service.post(NotificationSeverity::Info, "test message");
    CHECK(receivedId == id);
  }

  TEST_CASE("NotificationService - dismiss publishes NotificationDismissed", "[app][runtime][notification]")
  {
    auto service = NotificationService{};

    auto id = service.post(NotificationSeverity::Warning, "warning");

    auto dismissedId = NotificationId{};
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

  TEST_CASE("NotificationService - dismissAll does not emit per-item events", "[app][runtime][notification]")
  {
    auto service = NotificationService{};

    service.post(NotificationSeverity::Info, "a");
    service.post(NotificationSeverity::Info, "b");

    int dismissedCount = 0;
    auto sub = service.onDismissed([&](auto const&) { ++dismissedCount; });

    service.dismissAll();
    CHECK(dismissedCount == 0);
  }
}
