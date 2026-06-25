// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/StateTypes.h>
#include <ao/uimodel/status/StatusSlotModel.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <utility>

namespace ao::uimodel::status::test
{
  namespace
  {
    rt::NotificationEntry notification(rt::NotificationSeverity severity,
                                       std::string message,
                                       bool sticky = false,
                                       std::optional<std::chrono::milliseconds> optTimeout = std::nullopt)
    {
      return rt::NotificationEntry{
        .id = rt::NotificationId{42},
        .severity = severity,
        .message = std::move(message),
        .sticky = sticky,
        .optTimeout = optTimeout,
      };
    }
  } // namespace

  TEST_CASE("StatusSlotModel - maps runtime events to display state", "[uimodel][status]")
  {
    auto model = StatusSlotModel{};

    SECTION("initial state shows selection info")
    {
      auto const state = model.initialState();

      CHECK(state.mode == StatusSlotDisplayMode::SelectionInfo);
      CHECK(state.message.empty());
      CHECK(!state.optAutoDismissTimeout);
    }

    SECTION("library task progress owns the slot")
    {
      auto const state = model.onLibraryTaskProgress("Scanning Albums", 0.625);

      CHECK(state.mode == StatusSlotDisplayMode::Progress);
      CHECK(state.message == "Scanning Albums");
      CHECK(state.progressFraction == 0.625);
      CHECK(!state.optSeverity);
      CHECK(!state.optAutoDismissTimeout);
    }

    SECTION("library completion reports no-op scans")
    {
      model.onLibraryTaskProgress("Preparing Library", 0.125);

      auto const state = model.onLibraryTaskCompleted(0);

      CHECK(state.mode == StatusSlotDisplayMode::Message);
      CHECK(state.message == "Library is up to date");
      CHECK(state.optSeverity == rt::NotificationSeverity::Info);
      CHECK(state.optAutoDismissTimeout == kStatusSlotDefaultAutoDismissTimeout);
    }

    SECTION("library completion reports added track count")
    {
      model.onLibraryTaskProgress("Reading Files", 0.875);

      auto const state = model.onLibraryTaskCompleted(17);

      CHECK(state.mode == StatusSlotDisplayMode::Message);
      CHECK(state.message == "Scan complete: 17 tracks added");
      CHECK(state.optSeverity == rt::NotificationSeverity::Info);
      CHECK(state.optAutoDismissTimeout == kStatusSlotDefaultAutoDismissTimeout);
    }

    SECTION("notification severity controls message styling and default timeout")
    {
      auto const optState =
        model.onNotificationPosted(notification(rt::NotificationSeverity::Warning, "Partial import"));
      REQUIRE(optState);

      CHECK(optState->mode == StatusSlotDisplayMode::Message);
      CHECK(optState->message == "Partial import");
      CHECK(optState->optSeverity == rt::NotificationSeverity::Warning);
      CHECK(optState->optAutoDismissTimeout == kStatusSlotDefaultAutoDismissTimeout);
      CHECK(statusSlotSeverityCssClass(rt::NotificationSeverity::Warning) == "ao-status-warning");
    }

    SECTION("sticky notifications do not auto dismiss")
    {
      auto const optState =
        model.onNotificationPosted(notification(rt::NotificationSeverity::Error, "Cannot write library", true));
      REQUIRE(optState);

      CHECK(optState->mode == StatusSlotDisplayMode::Message);
      CHECK(optState->optSeverity == rt::NotificationSeverity::Error);
      CHECK(!optState->optAutoDismissTimeout);
      CHECK(statusSlotSeverityCssClass(rt::NotificationSeverity::Error) == "ao-status-error");
    }

    SECTION("notification can provide a custom auto-dismiss timeout")
    {
      auto const optState = model.onNotificationPosted(
        notification(rt::NotificationSeverity::Info, "Saved playlist", false, std::chrono::milliseconds{1500}));
      REQUIRE(optState);

      CHECK(optState->optAutoDismissTimeout == std::chrono::milliseconds{1500});
      CHECK(statusSlotSeverityCssClass(rt::NotificationSeverity::Info) == "ao-status-info");
    }

    SECTION("notifications posted during a task are deferred until completion")
    {
      auto const progressState = model.onLibraryTaskProgress("Importing Library", 0.4);
      REQUIRE(progressState.mode == StatusSlotDisplayMode::Progress);

      auto const optImmediateState =
        model.onNotificationPosted(notification(rt::NotificationSeverity::Error, "Import failed", true));
      CHECK(!optImmediateState);

      auto const completedState = model.onLibraryTaskCompleted(9);

      CHECK(completedState.mode == StatusSlotDisplayMode::Message);
      CHECK(completedState.message == "Import failed");
      CHECK(completedState.optSeverity == rt::NotificationSeverity::Error);
      CHECK(!completedState.optAutoDismissTimeout);
    }

    SECTION("auto dismiss restores selection info")
    {
      CHECK(model.onNotificationPosted(notification(rt::NotificationSeverity::Info, "Saved playlist")));

      auto const state = model.onAutoDismiss();

      CHECK(state.mode == StatusSlotDisplayMode::SelectionInfo);
      CHECK(state.message.empty());
      CHECK(!state.optSeverity);
    }

    SECTION("unknown severity has no css class")
    {
      auto const invalidSeverity = static_cast<rt::NotificationSeverity>(255);

      CHECK(statusSlotSeverityCssClass(invalidSeverity).empty());
    }
  }
} // namespace ao::uimodel::status::test
