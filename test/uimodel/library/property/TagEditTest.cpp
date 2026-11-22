// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/property/TagEdit.h>

#include "test/MessageCatalogTestSupport.h"
#include "test/runtime/AsyncTestSupport.h"
#include "test/uimodel/library/track/TrackAuthoringTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibrarySnapshot.h>
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
      auto res = TrackAuthoringSession::begin(fixture.library(), trackIds);
      REQUIRE(res);
      return std::move(*res);
    }
  } // namespace

  TEST_CASE("Common tag edits - reordered or reentered equivalent tokens are not additions or removals")
  {
    auto const baseline = std::vector<std::string>{"Old", "Café", "Live"};
    auto current = std::vector<std::string>{"Live", "Old"};
    REQUIRE(appendCommonTag(current, "Cafe\u0301"));
    CHECK(diffCommonTags(baseline, current).empty());

    std::erase(current, "Old");
    REQUIRE(appendCommonTag(current, "Etude"));
    REQUIRE(appendCommonTag(current, "E\u0301tude"));
    auto const duplicateRes = appendCommonTag(current, "Étude");
    REQUIRE(duplicateRes);
    CHECK_FALSE(*duplicateRes);
    auto const changes = diffCommonTags(baseline, current);
    CHECK(changes.tagsToAdd == std::vector<std::string>{"Etude", "Étude"});
    CHECK(changes.tagsToRemove == std::vector<std::string>{"Old"});
  }

  TEST_CASE("Common tag edits - promoting one partial tag leaves other partial memberships alone")
  {
    auto fixture = TrackAuthoringFixture{2};
    auto const ids = fixture.trackIds();
    auto first = beginSession(fixture, ids.first(1));
    REQUIRE(fixture.runTask(first.submitTagsAsync({"Private", "Promote"}, {})));

    auto baseline = std::vector<std::string>{};
    auto session = [&]
    {
      auto snapshot = fixture.library().snapshot();
      auto boundRes = TrackAuthoringSession::begin(fixture.library(), ids, snapshot);
      REQUIRE(boundRes);
      baseline = snapshot.selectionTags(boundRes->targetIds());
      return std::move(*boundRes);
    }();
    auto current = baseline;
    REQUIRE(appendCommonTag(current, "Promote"));
    auto changes = diffCommonTags(baseline, current);
    auto const revision = session.boundRevision();
    auto patch = rt::TrackPropertiesPatch{.metadata = {.optTitle = "Both"},
                                          .tagsToAdd = std::move(changes.tagsToAdd),
                                          .tagsToRemove = std::move(changes.tagsToRemove)};
    auto const res = fixture.runTask(session.submitPropertiesAsync(std::move(patch)));
    REQUIRE(res);
    CHECK(res->status == rt::AuthoringStatus::Applied);
    CHECK(session.boundRevision() == revision + 1);
    CHECK(fixture.title(ids.front()) == "Both");
    CHECK(fixture.title(ids.back()) == "Both");
    CHECK(fixture.tags(ids.front()) == std::vector<std::string>{"Private", "Promote"});
    CHECK(fixture.tags(ids.back()) == std::vector<std::string>{"Promote"});
  }

  TEST_CASE("Common tag edits - invalid input leaves already admitted tags available for editing")
  {
    auto tags = std::vector<std::string>{"Keep"};
    auto const res = appendCommonTag(tags, "Bad\x80");
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::InvalidInput);
    CHECK(tags == std::vector<std::string>{"Keep"});
  }

  TEST_CASE("applyTagEdit reports tag mutations for a bound authoring session", "[concurrency]")
  {
    auto fixture = TrackAuthoringFixture{2};
    auto const& textCatalog = ao::test::englishMessageCatalog();
    auto const trackId = fixture.trackIds()[0];
    auto const trackId2 = fixture.trackIds()[1];

    SECTION("empty tag changes do not submit a mutation")
    {
      auto session = beginSession(fixture, std::array{trackId});
      auto const res = fixture.runTask(applyTagEditAsync(session, textCatalog, {}, {}));

      REQUIRE(res);
      CHECK(res->status == rt::AuthoringStatus::NoOp);
      CHECK(res->notificationText.empty());
      CHECK(fixture.tags(trackId).empty());
    }

    SECTION("a submitted semantic no-op keeps the session current")
    {
      auto session = beginSession(fixture, std::array{trackId});
      auto const setupRes = fixture.runTask(session.submitTagsAsync({"Tag1"}, {}));
      REQUIRE(setupRes);
      REQUIRE(setupRes->status == rt::AuthoringStatus::Applied);
      auto const revision = session.boundRevision();

      auto const res = fixture.runTask(applyTagEditAsync(session, textCatalog, {"Tag1"}, {}));

      REQUIRE(res);
      CHECK(res->status == rt::AuthoringStatus::NoOp);
      CHECK(res->notificationText.empty());
      CHECK(session.isCurrent());
      CHECK(session.boundRevision() == revision);
      CHECK(fixture.library().authoringAvailability().libraryRevision == revision);
      CHECK(fixture.tags(trackId) == std::vector<std::string>{"Tag1"});
    }

    SECTION("an intervening commit reports the edit as stale")
    {
      auto session = beginSession(fixture, std::array{trackId});
      REQUIRE(fixture.runTask(fixture.library().commands().createListAsync(rt::ListDraft{.name = "Unrelated"})));

      auto const res = fixture.runTask(applyTagEditAsync(session, textCatalog, {"Tag1"}, {}));

      REQUIRE(res);
      CHECK(res->status == rt::AuthoringStatus::Stale);
      CHECK(res->notificationText == "Library changed while the tag editor was open. Reload and try again.");
      CHECK(fixture.tags(trackId).empty());
    }

    SECTION("closed runtime authoring reports the edit as unavailable")
    {
      auto session = beginSession(fixture, std::array{trackId});
      fixture.library().beginClosing();

      auto const res = fixture.runTask(applyTagEditAsync(session, textCatalog, {"Tag1"}, {}));

      REQUIRE(res);
      CHECK(res->status == rt::AuthoringStatus::Unavailable);
      CHECK(res->notificationText == "Library changed while the tag editor was open. Reload and try again.");
      CHECK_FALSE(session.isCurrent());
      CHECK(fixture.tags(trackId).empty());
    }

    SECTION("overlapping add and remove input propagates InvalidInput")
    {
      auto session = beginSession(fixture, std::array{trackId});

      auto const res = fixture.runTask(applyTagEditAsync(session, textCatalog, {"Tag1"}, {"Tag1"}));

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidInput);
      CHECK(res.error().message == "A tag cannot be both added and removed");
      CHECK_FALSE(session.isCurrent());
      CHECK(fixture.tags(trackId).empty());
    }

    SECTION("lane contention reports that the edit can be retried")
    {
      auto session = beginSession(fixture, std::array{trackId});
      auto createCompletedPtr = std::make_shared<std::atomic_bool>(false);
      auto createFuture = fixture.runtime().spawn(rt::test::flagCompletionAsync(
        createCompletedPtr, fixture.library().commands().createListAsync(rt::ListDraft{.name = "Unrelated"})));
      REQUIRE(fixture.executor().tryWaitUntilQueued());

      auto editCompletedPtr = std::make_shared<std::atomic_bool>(false);
      auto editFuture = fixture.runtime().spawn(
        rt::test::flagCompletionAsync(editCompletedPtr, applyTagEditAsync(session, textCatalog, {"Tag1"}, {})));
      REQUIRE(fixture.executor().tryWaitUntilQueuedCount(2));
      REQUIRE(fixture.executor().tryDrainUntil([&] { return createCompletedPtr->load() && editCompletedPtr->load(); }));

      REQUIRE(createFuture.get());
      auto const res = editFuture.get();
      REQUIRE(res);
      CHECK(res->status == rt::AuthoringStatus::Busy);
      CHECK(res->notificationText == "Library is busy. Try again.");
      CHECK(fixture.tags(trackId).empty());
    }

    SECTION("adding a single tag mutates the bound track and reports the count")
    {
      auto session = beginSession(fixture, std::array{trackId});
      auto const res = fixture.runTask(applyTagEditAsync(session, textCatalog, {"Tag1"}, {}));

      REQUIRE(res);
      CHECK(res->status == rt::AuthoringStatus::Applied);
      CHECK(res->notificationText == "Tags added 1 for 1 track");
      CHECK(fixture.tags(trackId) == std::vector<std::string>{"Tag1"});
      CHECK(fixture.tags(trackId2).empty());
    }

    SECTION("removing a tag mutates every bound track and reports the count")
    {
      auto const targetIds = std::array{trackId, trackId2};
      auto session = beginSession(fixture, targetIds);
      REQUIRE(fixture.runTask(session.submitTagsAsync({"Tag1"}, {})));

      auto const res = fixture.runTask(applyTagEditAsync(session, textCatalog, {}, {"Tag1"}));

      REQUIRE(res);
      CHECK(res->status == rt::AuthoringStatus::Applied);
      CHECK(res->notificationText == "Tags removed 1 for 2 tracks");
      CHECK(fixture.tags(trackId).empty());
      CHECK(fixture.tags(trackId2).empty());
    }

    SECTION("adding and removing tags remains one atomic session submission")
    {
      auto session = beginSession(fixture, std::array{trackId});
      REQUIRE(fixture.runTask(session.submitTagsAsync({"OldTag"}, {})));

      auto const res = fixture.runTask(applyTagEditAsync(session, textCatalog, {"NewTag"}, {"OldTag"}));

      REQUIRE(res);
      CHECK(res->status == rt::AuthoringStatus::Applied);
      CHECK(res->notificationText == "Tags added 1 and removed 1 for 1 track");
      CHECK(fixture.tags(trackId) == std::vector<std::string>{"NewTag"});
    }
  }
} // namespace ao::uimodel::test
