// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/ViewServiceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/ListStore.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/uimodel/library/list/ListOrderAuthoringSession.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
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
        , listId{ao::test::requireValue(runtime.writer().createList(rt::LibraryWriter::ListDraft{
            .name = "Ordered",
          }))}
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
  } // namespace

  TEST_CASE("ListOrderAuthoringSession - relative move commits the complete source order",
            "[uimodel][unit][list][list-order]")
  {
    auto fixture = SessionFixture{};
    auto const viewId = fixture.open();
    auto sessionResult =
      ListOrderAuthoringSession::begin(fixture.runtime.writerFixture.library(), fixture.runtime.service, viewId);
    REQUIRE(sessionResult);
    auto& session = **sessionResult;

    CHECK(std::vector<TrackId>{session.effectiveTrackIds().begin(), session.effectiveTrackIds().end()} ==
          std::vector{fixture.first, fixture.second, fixture.third});
    auto const result = session.moveDown(std::array{fixture.first});

    REQUIRE(result);
    CHECK(result->status == rt::ListOrderAuthoringStatus::Applied);
    CHECK(fixture.storedOrder() == std::vector{fixture.second, fixture.first, fixture.third});
    CHECK_FALSE(session.isCurrent());
  }

  TEST_CASE("ListOrderAuthoringSession - quick filter permits absolute but not relative moves",
            "[uimodel][unit][list][list-order]")
  {
    auto fixture = SessionFixture{};
    auto const viewId = fixture.open("true");
    auto sessionResult =
      ListOrderAuthoringSession::begin(fixture.runtime.writerFixture.library(), fixture.runtime.service, viewId);
    REQUIRE(sessionResult);
    auto& session = **sessionResult;

    CHECK(session.capabilities().canAbsoluteMove);
    CHECK_FALSE(session.capabilities().canGapMove);
    CHECK_FALSE(session.capabilities().canRelativeMove);

    auto const relative = session.moveUp(std::array{fixture.second});
    REQUIRE_FALSE(relative);
    CHECK(relative.error().code == Error::Code::InvalidState);

    auto const absolute = session.moveToTop(std::array{fixture.third});
    REQUIRE(absolute);
    CHECK(absolute->status == rt::ListOrderAuthoringStatus::Applied);
    CHECK(fixture.storedOrder() == std::vector{fixture.third, fixture.first, fixture.second});
  }

  TEST_CASE("ListOrderAuthoringSession - presentation, filter, and view lifecycle invalidate the binding",
            "[uimodel][unit][list][list-order]")
  {
    auto fixture = SessionFixture{};

    SECTION("presentation")
    {
      auto const viewId = fixture.open();
      auto sessionPtr = ao::test::requireValue(
        ListOrderAuthoringSession::begin(fixture.runtime.writerFixture.library(), fixture.runtime.service, viewId));
      REQUIRE(fixture.runtime.service.setPresentation(viewId, rt::defaultTrackPresentationSpec()));
      CHECK_FALSE(sessionPtr->isCurrent());
    }

    SECTION("quick filter")
    {
      auto const viewId = fixture.open();
      auto sessionPtr = ao::test::requireValue(
        ListOrderAuthoringSession::begin(fixture.runtime.writerFixture.library(), fixture.runtime.service, viewId));
      REQUIRE(fixture.runtime.service.setFilter(viewId, "true"));
      CHECK_FALSE(sessionPtr->isCurrent());
    }

    SECTION("view close")
    {
      auto const viewId = fixture.open();
      auto sessionPtr = ao::test::requireValue(
        ListOrderAuthoringSession::begin(fixture.runtime.writerFixture.library(), fixture.runtime.service, viewId));
      REQUIRE(fixture.runtime.workspace.closeView(viewId));
      CHECK_FALSE(sessionPtr->isCurrent());
    }
  }
} // namespace ao::uimodel::test
