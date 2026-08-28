// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/runtime/ViewServiceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/ListStore.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/uimodel/library/list/ListOrderSession.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  namespace
  {
    struct SessionFixture final
    {
      SessionFixture()
        : first{runtime.addTrack(library::test::TrackSpec{.title = "First"})}
        , second{runtime.addTrack(library::test::TrackSpec{.title = "Second"})}
        , third{runtime.addTrack(library::test::TrackSpec{.title = "Third"})}
        , listId{ao::test::requireValue(runtime.commandsFixture.runTask(runtime.commands().createList(rt::ListDraft{
            .name = "Ordered",
          })))}
      {
      }

      rt::ViewId open(std::string filterExpression = {})
      {
        auto const* manual = rt::builtinTrackPresentationPreset(rt::kListOrderTrackPresentationId);
        REQUIRE(manual != nullptr);
        return runtime.requireView(rt::TrackListViewConfig{
          .listId = listId,
          .filterExpression = std::move(filterExpression),
          .optPresentation = manual->spec,
        });
      }

      std::vector<TrackId> storedOrder() const
      {
        auto transaction = runtime.libraryFixture.library().readTransaction();
        auto const optList = runtime.libraryFixture.library().lists().reader(transaction).get(listId);
        REQUIRE(optList);
        return {optList->orderTrackIds().begin(), optList->orderTrackIds().end()};
      }

      rt::test::ViewServiceFixture runtime;
      TrackId first;
      TrackId second;
      TrackId third;
      ListId listId;
    };

    struct PendingSessionFixture final
    {
      PendingSessionFixture()
        : changes{executor, 0, "test-library"}
        , commandsFixture{libraryFixture.library(), changes, executor}
        , cachePtr{std::make_unique<rt::TrackSourceCache>(libraryFixture.library(), changes)}
        , service{executor, libraryFixture.library(), *cachePtr, changes}
        , workspace{executor, service, changes}
      {
        first = commandsFixture.addTrack(library::test::TrackSpec{.title = "First"});
        second = commandsFixture.addTrack(library::test::TrackSpec{.title = "Second"});
        listId = ao::test::requireValue(
          commandsFixture.runTask(commandsFixture.commands().createList(rt::ListDraft{.name = "Ordered"})));
      }

      rt::ViewId open()
      {
        auto const* manual = rt::builtinTrackPresentationPreset(rt::kListOrderTrackPresentationId);
        REQUIRE(manual != nullptr);
        auto const viewId = ao::test::requireValue(workspace.navigate(rt::NavigationRequest{
          .target = rt::FilteredListTarget{.listId = listId, .filterExpression = {}},
          .optPresentation = rt::NavigationPresentation{.spec = manual->spec},
        }));
        executor.runUntilIdle();
        return viewId;
      }

      rt::test::MusicLibraryFixture libraryFixture;
      rt::test::ManualExecutor executor;
      rt::LibraryChanges changes;
      rt::test::LibraryCommandsFixture commandsFixture;
      std::unique_ptr<rt::TrackSourceCache> cachePtr;
      rt::ViewService service;
      rt::WorkspaceService workspace;
      TrackId first = kInvalidTrackId;
      TrackId second = kInvalidTrackId;
      ListId listId = kInvalidListId;
    };
  } // namespace

  TEST_CASE("ListOrderAuthoringSession - relative move commits the complete source order",
            "[uimodel][unit][list][list-order]")
  {
    auto fixture = SessionFixture{};
    auto const viewId = fixture.open();
    auto sessionRes = ListOrderAuthoringSession::begin(
      fixture.runtime.commandsFixture.library(), fixture.runtime.service, viewId, ao::test::englishMessageCatalog());
    REQUIRE(sessionRes);
    auto& session = **sessionRes;

    CHECK(std::vector<TrackId>{session.effectiveTrackIds().begin(), session.effectiveTrackIds().end()} ==
          std::vector{fixture.first, fixture.second, fixture.third});
    auto const result = fixture.runtime.commandsFixture.runTask(session.moveDown({fixture.first}));

    REQUIRE(result);
    CHECK(result->status == rt::AuthoringStatus::Applied);
    CHECK(fixture.storedOrder() == std::vector{fixture.second, fixture.first, fixture.third});
    CHECK_FALSE(session.isCurrent());
  }

  TEST_CASE("ListOrderAuthoringSession - quick filter permits absolute but not relative moves",
            "[uimodel][unit][list][list-order]")
  {
    auto fixture = SessionFixture{};
    auto const viewId = fixture.open("true");
    auto sessionRes = ListOrderAuthoringSession::begin(
      fixture.runtime.commandsFixture.library(), fixture.runtime.service, viewId, ao::test::englishMessageCatalog());
    REQUIRE(sessionRes);
    auto& session = **sessionRes;

    CHECK(session.capabilities().canAbsoluteMove);
    CHECK_FALSE(session.capabilities().canGapMove);
    CHECK_FALSE(session.capabilities().canRelativeMove);

    auto const relativeRes = fixture.runtime.commandsFixture.runTask(session.moveUp({fixture.second}));
    REQUIRE_FALSE(relativeRes);
    CHECK(relativeRes.error().code == Error::Code::InvalidState);

    auto const absoluteRes = fixture.runtime.commandsFixture.runTask(session.moveToTop({fixture.third}));
    REQUIRE(absoluteRes);
    CHECK(absoluteRes->status == rt::AuthoringStatus::Applied);
    CHECK(fixture.storedOrder() == std::vector{fixture.third, fixture.first, fixture.second});
  }

  TEST_CASE("ListOrderAuthoringSession - presentation, filter, and view lifecycle invalidate the binding",
            "[uimodel][unit][list][list-order]")
  {
    auto fixture = SessionFixture{};

    SECTION("presentation")
    {
      auto const viewId = fixture.open();
      auto sessionPtr = ao::test::requireValue(ListOrderAuthoringSession::begin(
        fixture.runtime.commandsFixture.library(), fixture.runtime.service, viewId, ao::test::englishMessageCatalog()));
      REQUIRE(fixture.runtime.service.setPresentation(viewId, rt::defaultTrackPresentationSpec()));
      fixture.runtime.drainCallbacks();
      CHECK_FALSE(sessionPtr->isCurrent());
    }

    SECTION("quick filter")
    {
      auto const viewId = fixture.open();
      auto sessionPtr = ao::test::requireValue(ListOrderAuthoringSession::begin(
        fixture.runtime.commandsFixture.library(), fixture.runtime.service, viewId, ao::test::englishMessageCatalog()));
      REQUIRE(fixture.runtime.service.setFilter(viewId, "true"));
      fixture.runtime.drainCallbacks();
      CHECK_FALSE(sessionPtr->isCurrent());
    }

    SECTION("view close")
    {
      auto const viewId = fixture.open();
      auto sessionPtr = ao::test::requireValue(ListOrderAuthoringSession::begin(
        fixture.runtime.commandsFixture.library(), fixture.runtime.service, viewId, ao::test::englishMessageCatalog()));
      REQUIRE(fixture.runtime.workspace.closeView(viewId));
      fixture.runtime.drainCallbacks();
      CHECK_FALSE(sessionPtr->isCurrent());
    }
  }

  TEST_CASE("ListOrderAuthoringSession - NoOp replays invalidation observed while submission is pending",
            "[uimodel][regression][list-order][concurrency]")
  {
    auto fixture = PendingSessionFixture{};
    auto const viewId = fixture.open();
    auto sessionRes = ListOrderAuthoringSession::begin(
      fixture.commandsFixture.library(), fixture.service, viewId, ao::test::englishMessageCatalog());
    REQUIRE(sessionRes);
    auto sessionPtr = std::move(*sessionRes);
    std::size_t invalidatedCount = 0;
    auto subscription = sessionPtr->onInvalidated([&invalidatedCount] noexcept { ++invalidatedCount; });
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto future =
      fixture.commandsFixture.runtime().spawn(rt::test::flagCompletion(completedPtr, sessionPtr->resetOrder()));
    REQUIRE(fixture.executor.waitUntilQueued());

    REQUIRE(fixture.service.setPresentation(viewId, rt::defaultTrackPresentationSpec()));
    CHECK(sessionPtr->isCurrent());
    REQUIRE(fixture.executor.drainUntil([&completedPtr] { return completedPtr->load(); }));

    auto result = future.get();
    REQUIRE(result);
    CHECK(result->status == rt::AuthoringStatus::NoOp);
    CHECK_FALSE(sessionPtr->isCurrent());
    CHECK(invalidatedCount == 1);
  }
} // namespace ao::uimodel::test
