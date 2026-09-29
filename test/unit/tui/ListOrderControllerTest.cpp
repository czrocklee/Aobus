// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/ListOrderController.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibrarySnapshot.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <memory>
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

    struct OrderFixture final
    {
      ao::test::TempDir tempDir{};
      rt::test::QueuedExecutor* executor = nullptr;
      std::unique_ptr<rt::AppRuntime> runtimePtr{rt::test::makeRuntime(tempDir, makeQueuedExecutor(executor))};

      TrackId first{rt::test::addRuntimeTrack(*runtimePtr, library::test::TrackSpec{.title = "First"})};
      TrackId second{rt::test::addRuntimeTrack(*runtimePtr, library::test::TrackSpec{.title = "Second"})};
      TrackId third{rt::test::addRuntimeTrack(*runtimePtr, library::test::TrackSpec{.title = "Third"})};
      ListId listId{};

      OrderFixture()
      {
        listId = ao::test::requireValue(
          rt::test::runRuntimeTask(*runtimePtr,
                                   runtimePtr->library().commands().createListAsync(
                                     rt::ListDraft{.name = "Ordered", .expression = "#ordered"})));
        auto const trackIds = std::array{first, second, third};
        auto targetsRes = runtimePtr->library().bindTrackTargets(trackIds);
        REQUIRE(targetsRes);
        ao::test::requireValue(rt::test::runRuntimeTask(
          *runtimePtr, runtimePtr->library().commands().addTracksToListAsync(listId, std::move(*targetsRes))));
        executor->drain();
      }

      /// Opens the saved List in the shipped flat unsorted Manual Order presentation.
      rt::ViewId openManualOrderView() const
      {
        auto const* const manual = rt::builtinTrackPresentationPreset(rt::kListOrderTrackPresentationId);
        REQUIRE(manual != nullptr);
        auto const viewId = ao::test::requireValue(runtimePtr->workspace().navigate(rt::NavigationRequest{
          .target = rt::FilteredListTarget{.listId = listId, .filterExpression = {}},
          .optPresentation = rt::NavigationPresentation{.spec = manual->spec},
        }));
        executor->drain();
        return viewId;
      }

      /// Opens the saved List grouped by album, which cannot carry a manual order.
      rt::ViewId openGroupedView() const
      {
        auto const* const albums = rt::builtinTrackPresentationPreset("albums");
        REQUIRE(albums != nullptr);
        auto const viewId = ao::test::requireValue(runtimePtr->workspace().navigate(rt::NavigationRequest{
          .target = rt::FilteredListTarget{.listId = listId, .filterExpression = {}},
          .optPresentation = rt::NavigationPresentation{.spec = albums->spec},
        }));
        executor->drain();
        return viewId;
      }

      std::vector<TrackId> storedOrder() const { return runtimePtr->library().snapshot().listOrderTrackIds(listId); }

      bool hasNotifications() const { return !runtimePtr->notifications().feed().entries.empty(); }

      std::size_t notificationCount() const { return runtimePtr->notifications().feed().entries.size(); }

      std::string lastMessage() const
      {
        auto const feed = runtimePtr->notifications().feed();
        REQUIRE_FALSE(feed.entries.empty());
        return std::get<std::string>(feed.entries.back().message);
      }

      rt::NotificationSeverity lastSeverity() const
      {
        auto const feed = runtimePtr->notifications().feed();
        REQUIRE_FALSE(feed.entries.empty());
        return feed.entries.back().severity;
      }

      ListOrderController makeController() const
      {
        return ListOrderController{
          runtimePtr->async(),
          runtimePtr->library(),
          runtimePtr->views(),
          runtimePtr->notifications(),
          ao::test::englishMessageCatalog(),
        };
      }
    };
  } // namespace

  TEST_CASE("ListOrderController - an applied relative move commits the order and reports its count",
            "[tui][integration][list-order][concurrency]")
  {
    auto fixture = OrderFixture{};
    auto const viewId = fixture.openManualOrderView();
    auto controller = fixture.makeController();

    controller.apply(ListOrderCommand::MoveDown, viewId, {fixture.first});
    REQUIRE(fixture.executor->tryDrainUntil([&fixture] { return fixture.hasNotifications(); }));

    CHECK(fixture.lastMessage() == "Moved 1 track in Manual Order.");
    CHECK(fixture.runtimePtr->notifications().feed().entries.back().lifetime.kind() ==
          rt::NotificationLifetimeKind::Transient);
    CHECK(fixture.storedOrder() == std::vector{fixture.second, fixture.first, fixture.third});
  }

  TEST_CASE("ListOrderController - a relative move of an edge selection is a reported no-op",
            "[tui][integration][list-order][concurrency]")
  {
    auto fixture = OrderFixture{};
    auto const viewId = fixture.openManualOrderView();
    auto controller = fixture.makeController();

    controller.apply(ListOrderCommand::MoveUp, viewId, {fixture.first});
    REQUIRE(fixture.executor->tryDrainUntil([&fixture] { return fixture.hasNotifications(); }));

    CHECK(fixture.lastMessage() == "Order unchanged.");
    // A no-op persists nothing, so the raw order stays unset.
    CHECK(fixture.storedOrder().empty());
  }

  TEST_CASE("ListOrderController - a command without order capability reports its blocking reason",
            "[tui][integration][list-order][concurrency]")
  {
    auto fixture = OrderFixture{};
    auto const viewId = fixture.openGroupedView();
    auto controller = fixture.makeController();

    controller.apply(ListOrderCommand::MoveDown, viewId, {fixture.first});
    REQUIRE(fixture.executor->tryDrainUntil([&fixture] { return fixture.hasNotifications(); }));

    // The reason is guidance for this keypress, so it passes like any other
    // transient outcome instead of pinning a warning a later successful move
    // would leave standing.
    CHECK(fixture.lastMessage() == "Choose Manual Order or another flat unsorted presentation to rearrange tracks.");
    CHECK(fixture.lastSeverity() == rt::NotificationSeverity::Info);
    CHECK(fixture.runtimePtr->notifications().feed().entries.back().lifetime.kind() ==
          rt::NotificationLifetimeKind::Transient);
    // The blocked command never reached a binding, so no rank was persisted.
    CHECK(fixture.storedOrder().empty());
  }

  TEST_CASE("ListOrderController - top and bottom move a marked set as one block",
            "[tui][integration][list-order][concurrency]")
  {
    auto fixture = OrderFixture{};
    auto const viewId = fixture.openManualOrderView();
    auto controller = fixture.makeController();

    controller.apply(ListOrderCommand::MoveToTop, viewId, {fixture.third});
    REQUIRE(fixture.executor->tryDrainUntil([&fixture] { return fixture.notificationCount() == 1; }));
    CHECK(fixture.lastMessage() == "Moved 1 track in Manual Order.");
    CHECK(fixture.storedOrder() == std::vector{fixture.third, fixture.first, fixture.second});

    // Two marks keep their relative order as they move to the bottom.
    controller.apply(ListOrderCommand::MoveToBottom, viewId, {fixture.third, fixture.first});
    REQUIRE(fixture.executor->tryDrainUntil([&fixture] { return fixture.notificationCount() == 2; }));
    CHECK(fixture.lastMessage() == "Moved 2 tracks in Manual Order.");
    CHECK(fixture.storedOrder() == std::vector{fixture.second, fixture.third, fixture.first});
  }

  TEST_CASE("ListOrderController - a relative move of several marks moves them together",
            "[tui][integration][list-order][concurrency]")
  {
    auto fixture = OrderFixture{};
    auto const viewId = fixture.openManualOrderView();
    auto controller = fixture.makeController();

    controller.apply(ListOrderCommand::MoveDown, viewId, {fixture.first, fixture.second});
    REQUIRE(fixture.executor->tryDrainUntil([&fixture] { return fixture.hasNotifications(); }));

    CHECK(fixture.lastMessage() == "Moved 2 tracks in Manual Order.");
    CHECK(fixture.storedOrder() == std::vector{fixture.third, fixture.first, fixture.second});
  }

  TEST_CASE("ListOrderController - reset forgets the saved positions and reports the count",
            "[tui][integration][list-order][concurrency]")
  {
    auto fixture = OrderFixture{};
    auto const viewId = fixture.openManualOrderView();
    auto controller = fixture.makeController();

    controller.apply(ListOrderCommand::MoveDown, viewId, {fixture.first});
    REQUIRE(fixture.executor->tryDrainUntil([&fixture] { return fixture.hasNotifications(); }));
    REQUIRE(fixture.storedOrder() == std::vector{fixture.second, fixture.first, fixture.third});

    controller.apply(ListOrderCommand::Reset, viewId, {});
    REQUIRE(fixture.executor->tryDrainUntil([&fixture] { return fixture.notificationCount() == 2; }));

    CHECK(fixture.lastMessage() == "Reset Manual Order and forgot 3 saved positions.");
    // Reset clears the saved rank sequence; the List falls back to its natural order.
    CHECK(fixture.storedOrder().empty());
  }

  TEST_CASE("ListOrderController - a view changed under a submitted command reports a stale order",
            "[tui][integration][list-order][concurrency]")
  {
    auto fixture = OrderFixture{};
    auto const viewId = fixture.openManualOrderView();
    auto controller = fixture.makeController();

    controller.apply(ListOrderCommand::MoveDown, viewId, {fixture.first});

    // The presentation changes after the command bound its order but before
    // the submission reached the library.
    auto const* const albums = rt::builtinTrackPresentationPreset("albums");
    REQUIRE(albums != nullptr);
    REQUIRE(fixture.runtimePtr->views().setPresentation(viewId, albums->spec));
    REQUIRE(fixture.executor->tryDrainUntil([&fixture] { return fixture.hasNotifications(); }));

    CHECK(fixture.notificationCount() == 1);
    CHECK(fixture.lastMessage() == "The List changed. Start the order action again.");
    CHECK(fixture.lastSeverity() == rt::NotificationSeverity::Info);
    CHECK(fixture.runtimePtr->notifications().feed().entries.back().lifetime.kind() ==
          rt::NotificationLifetimeKind::Transient);
    CHECK(fixture.storedOrder().empty());
  }

  TEST_CASE("ListOrderController - a rejected command stays behind as a warning",
            "[tui][integration][list-order][concurrency]")
  {
    auto fixture = OrderFixture{};
    auto const outsider = rt::test::addRuntimeTrack(*fixture.runtimePtr, library::test::TrackSpec{.title = "Outside"});
    auto const viewId = fixture.openManualOrderView();
    auto controller = fixture.makeController();

    controller.apply(ListOrderCommand::MoveDown, viewId, {outsider});
    REQUIRE(fixture.executor->tryDrainUntil([&fixture] { return fixture.hasNotifications(); }));

    CHECK(fixture.lastSeverity() == rt::NotificationSeverity::Warning);
    CHECK(fixture.runtimePtr->notifications().feed().entries.back().lifetime.kind() ==
          rt::NotificationLifetimeKind::History);
    CHECK(fixture.storedOrder().empty());

    // The failed command released the in-flight gate, so the next one runs.
    controller.apply(ListOrderCommand::MoveDown, viewId, {fixture.first});
    REQUIRE(fixture.executor->tryDrainUntil([&fixture] { return fixture.notificationCount() == 2; }));
    CHECK(fixture.lastMessage() == "Moved 1 track in Manual Order.");
  }

  TEST_CASE("ListOrderController - a second command while one is in flight is dropped",
            "[tui][integration][list-order][concurrency]")
  {
    auto fixture = OrderFixture{};
    auto const viewId = fixture.openManualOrderView();
    auto controller = fixture.makeController();

    controller.apply(ListOrderCommand::MoveDown, viewId, {fixture.first});
    controller.apply(ListOrderCommand::MoveDown, viewId, {fixture.first});

    REQUIRE(fixture.executor->tryDrainUntil([&fixture] { return fixture.hasNotifications(); }));
    fixture.executor->drain();

    // Exactly one move committed: the second command never reached a binding,
    // so it left no notification of its own and no extra order step.
    CHECK(fixture.notificationCount() == 1);
    CHECK(fixture.lastMessage() == "Moved 1 track in Manual Order.");
    CHECK(fixture.storedOrder() == std::vector{fixture.second, fixture.first, fixture.third});
  }

  TEST_CASE("ListOrderController - a later command runs after the previous one settled",
            "[tui][integration][list-order][concurrency]")
  {
    auto fixture = OrderFixture{};
    auto const viewId = fixture.openManualOrderView();
    auto controller = fixture.makeController();

    controller.apply(ListOrderCommand::MoveDown, viewId, {fixture.first});
    REQUIRE(fixture.executor->tryDrainUntil([&fixture] { return fixture.hasNotifications(); }));

    controller.apply(ListOrderCommand::MoveDown, viewId, {fixture.first});
    REQUIRE(fixture.executor->tryDrainUntil([&fixture] { return fixture.notificationCount() == 2; }));

    CHECK(fixture.lastMessage() == "Moved 1 track in Manual Order.");
    CHECK(fixture.storedOrder() == std::vector{fixture.second, fixture.third, fixture.first});
  }

  TEST_CASE("ListOrderController - a submission outlives its destroyed controller",
            "[tui][integration][list-order][concurrency]")
  {
    auto fixture = OrderFixture{};
    auto const viewId = fixture.openManualOrderView();

    {
      auto controller = fixture.makeController();
      controller.apply(ListOrderCommand::MoveDown, viewId, {fixture.first});

      // The write commits before its outcome settles, so the controller is
      // destroyed below while the submission is still in flight.
      REQUIRE(fixture.executor->tryDrainUntil([&fixture] { return !fixture.storedOrder().empty(); }));
    }

    REQUIRE(fixture.executor->tryDrainUntil([&fixture] { return fixture.hasNotifications(); }));

    CHECK(fixture.notificationCount() == 1);
    CHECK(fixture.lastMessage() == "Moved 1 track in Manual Order.");
    CHECK(fixture.lastSeverity() == rt::NotificationSeverity::Info);
    CHECK(fixture.storedOrder() == std::vector{fixture.second, fixture.first, fixture.third});
  }
} // namespace ao::tui::test
