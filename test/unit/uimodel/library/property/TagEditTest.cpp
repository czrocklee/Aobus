// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/property/TagEdit.h>

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/uimodel/library/track/TrackAuthoringTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  namespace
  {
    TrackAuthoringSession beginSession(TrackAuthoringFixture& fixture, std::span<TrackId const> trackIds)
    {
      auto result = TrackAuthoringSession::begin(fixture.library(), trackIds);
      REQUIRE(result);
      return std::move(*result);
    }
  } // namespace

  TEST_CASE("applyTagEdit reports tag mutations for a bound authoring session", "[uimodel][unit][tag-edit]")
  {
    auto fixture = TrackAuthoringFixture{2};
    auto const& textCatalog = ao::test::englishMessageCatalog();
    auto const trackId = fixture.trackIds()[0];
    auto const trackId2 = fixture.trackIds()[1];

    SECTION("empty tag changes do not submit a mutation")
    {
      auto session = beginSession(fixture, std::array{trackId});
      auto const result = fixture.runTask(applyTagEdit(session, textCatalog, {}, {}));

      REQUIRE(result);
      CHECK(result->status == rt::AuthoringStatus::NoOp);
      CHECK(result->notificationText.empty());
      CHECK(fixture.tags(trackId).empty());
    }

    SECTION("an intervening commit reports the edit as stale")
    {
      auto session = beginSession(fixture, std::array{trackId});
      REQUIRE(fixture.runTask(fixture.library().commands().createList(rt::ListDraft{.name = "Unrelated"})));

      auto const result = fixture.runTask(applyTagEdit(session, textCatalog, {"Tag1"}, {}));

      REQUIRE(result);
      CHECK(result->status == rt::AuthoringStatus::Stale);
      CHECK(result->notificationText == "Library changed while the tag editor was open. Reload and try again.");
      CHECK(fixture.tags(trackId).empty());
    }

    SECTION("lane contention reports that the edit can be retried")
    {
      auto session = beginSession(fixture, std::array{trackId});
      auto createCompletedPtr = std::make_shared<std::atomic_bool>(false);
      auto createFuture = fixture.runtime().spawn(rt::test::flagCompletion(
        createCompletedPtr, fixture.library().commands().createList(rt::ListDraft{.name = "Unrelated"})));
      REQUIRE(fixture.executor().waitUntilQueued());

      auto editCompletedPtr = std::make_shared<std::atomic_bool>(false);
      auto editFuture = fixture.runtime().spawn(
        rt::test::flagCompletion(editCompletedPtr, applyTagEdit(session, textCatalog, {"Tag1"}, {})));
      REQUIRE(fixture.executor().waitUntilQueuedCount(2));
      REQUIRE(fixture.executor().drainUntil([&] { return createCompletedPtr->load() && editCompletedPtr->load(); }));

      REQUIRE(createFuture.get());
      auto const result = editFuture.get();
      REQUIRE(result);
      CHECK(result->status == rt::AuthoringStatus::Busy);
      CHECK(result->notificationText == "Library is busy. Try again.");
      CHECK(fixture.tags(trackId).empty());
    }

    SECTION("adding a single tag mutates the bound track and reports the count")
    {
      auto session = beginSession(fixture, std::array{trackId});
      auto const result = fixture.runTask(applyTagEdit(session, textCatalog, {"Tag1"}, {}));

      REQUIRE(result);
      CHECK(result->status == rt::AuthoringStatus::Applied);
      CHECK(result->notificationText == "Tags added 1 for 1 track");
      CHECK(fixture.tags(trackId) == std::vector<std::string>{"Tag1"});
      CHECK(fixture.tags(trackId2).empty());
    }

    SECTION("removing a tag mutates every bound track and reports the count")
    {
      auto const targetIds = std::array{trackId, trackId2};
      auto session = beginSession(fixture, targetIds);
      REQUIRE(fixture.runTask(session.submitTags({"Tag1"}, {})));

      auto const result = fixture.runTask(applyTagEdit(session, textCatalog, {}, {"Tag1"}));

      REQUIRE(result);
      CHECK(result->status == rt::AuthoringStatus::Applied);
      CHECK(result->notificationText == "Tags removed 1 for 2 tracks");
      CHECK(fixture.tags(trackId).empty());
      CHECK(fixture.tags(trackId2).empty());
    }

    SECTION("adding and removing tags remains one atomic session submission")
    {
      auto session = beginSession(fixture, std::array{trackId});
      REQUIRE(fixture.runTask(session.submitTags({"OldTag"}, {})));

      auto const result = fixture.runTask(applyTagEdit(session, textCatalog, {"NewTag"}, {"OldTag"}));

      REQUIRE(result);
      CHECK(result->status == rt::AuthoringStatus::Applied);
      CHECK(result->notificationText == "Tags added 1 and removed 1 for 1 track");
      CHECK(fixture.tags(trackId) == std::vector<std::string>{"NewTag"});
    }
  }
} // namespace ao::uimodel::test
