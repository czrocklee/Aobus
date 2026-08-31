// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/uimodel/library/track/TrackAuthoringTestSupport.h"
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("TrackAuthoringSession - owns stable targets and becomes stale after another commit",
            "[uimodel][unit][library-authoring]")
  {
    auto fixture = TrackAuthoringFixture{2};
    auto const targetIds = std::array{fixture.trackIds()[1], fixture.trackIds()[0]};
    auto sessionRes = TrackAuthoringSession::begin(fixture.library(), targetIds);
    REQUIRE(sessionRes);
    auto session = std::move(*sessionRes);

    CHECK(std::ranges::equal(session.targetIds(), targetIds));
    CHECK(session.isCurrent());

    std::size_t invalidatedCount = 0;
    auto subscription = session.onInvalidated([&invalidatedCount] noexcept { ++invalidatedCount; });
    auto patch = rt::MetadataPatch{.optTitle = "Applied"};
    auto submitRes = fixture.runTask(session.submitMetadata(patch));

    REQUIRE(submitRes);
    CHECK(submitRes->status == rt::AuthoringStatus::Applied);
    CHECK(session.isCurrent());
    CHECK(invalidatedCount == 0);
    CHECK(fixture.title(targetIds[0]) == "Applied");
    CHECK(fixture.title(targetIds[1]) == "Applied");

    REQUIRE(fixture.runTask(fixture.library().commands().createList(rt::ListDraft{.name = "Unrelated"})));
    CHECK_FALSE(session.isCurrent());
    CHECK(invalidatedCount == 1);

    patch.optTitle = "Must not apply";
    submitRes = fixture.runTask(session.submitMetadata(patch));
    REQUIRE(submitRes);
    CHECK(submitRes->status == rt::AuthoringStatus::Stale);
    CHECK(fixture.title(targetIds[0]) == "Applied");
  }

  TEST_CASE("TrackAuthoringSession - semantic no-op keeps the binding usable", "[uimodel][unit][library-authoring]")
  {
    auto fixture = TrackAuthoringFixture{1};
    auto sessionRes = TrackAuthoringSession::begin(fixture.library(), fixture.trackIds());
    REQUIRE(sessionRes);
    auto session = std::move(*sessionRes);

    auto submitRes = fixture.runTask(session.submitMetadata(rt::MetadataPatch{.optTitle = "Old Title"}));
    REQUIRE(submitRes);
    CHECK(submitRes->status == rt::AuthoringStatus::NoOp);
    CHECK(session.isCurrent());

    submitRes = fixture.runTask(session.submitMetadata(rt::MetadataPatch{.optTitle = "Now changed"}));
    REQUIRE(submitRes);
    CHECK(submitRes->status == rt::AuthoringStatus::Applied);
    CHECK(fixture.title(fixture.trackIds().front()) == "Now changed");
  }

  TEST_CASE("TrackAuthoringSession - Properties submission advances one binding for metadata and tags",
            "[uimodel][regression][library-authoring]")
  {
    auto fixture = TrackAuthoringFixture{1};
    auto sessionRes = TrackAuthoringSession::begin(fixture.library(), fixture.trackIds());
    REQUIRE(sessionRes);
    auto session = std::move(*sessionRes);

    auto const submitRes = fixture.runTask(session.submitProperties(rt::TrackPropertiesPatch{
      .metadata = rt::MetadataPatch{.optTitle = "Together"},
      .tagsToAdd = {"Favorite"},
    }));

    REQUIRE(submitRes);
    CHECK(submitRes->status == rt::AuthoringStatus::Applied);
    CHECK(session.isCurrent());
    CHECK(fixture.title(fixture.trackIds().front()) == "Together");
    CHECK(fixture.tags(fixture.trackIds().front()) == std::vector<std::string>{"Favorite"});
  }

  TEST_CASE("TrackAuthoringSession - a tag commit stales other sessions bound to the old revision",
            "[uimodel][unit][library-authoring]")
  {
    auto fixture = TrackAuthoringFixture{1};
    auto firstRes = TrackAuthoringSession::begin(fixture.library(), fixture.trackIds());
    auto secondRes = TrackAuthoringSession::begin(fixture.library(), fixture.trackIds());
    REQUIRE(firstRes);
    REQUIRE(secondRes);
    auto first = std::move(*firstRes);
    auto second = std::move(*secondRes);

    auto submitRes = fixture.runTask(first.submitTags({"First"}, {}));

    REQUIRE(submitRes);
    CHECK(submitRes->status == rt::AuthoringStatus::Applied);
    CHECK(first.isCurrent());
    CHECK_FALSE(second.isCurrent());
    CHECK(fixture.tags(fixture.trackIds().front()) == std::vector<std::string>{"First"});

    submitRes = fixture.runTask(second.submitTags({"Second"}, {}));
    REQUIRE(submitRes);
    CHECK(submitRes->status == rt::AuthoringStatus::Stale);
    CHECK(fixture.tags(fixture.trackIds().front()) == std::vector<std::string>{"First"});
  }

  TEST_CASE("TrackAuthoringSession - Busy reconciles a revision delivered while submission is pending",
            "[uimodel][regression][library-authoring][concurrency]")
  {
    auto fixture = TrackAuthoringFixture{1};
    auto sessionRes = TrackAuthoringSession::begin(fixture.library(), fixture.trackIds());
    REQUIRE(sessionRes);
    auto session = std::move(*sessionRes);
    std::size_t invalidatedCount = 0;
    auto subscription = session.onInvalidated([&invalidatedCount] noexcept { ++invalidatedCount; });

    auto createCompletedPtr = std::make_shared<std::atomic_bool>(false);
    auto createFuture = fixture.runtime().spawn(rt::test::flagCompletion(
      createCompletedPtr, fixture.library().commands().createList(rt::ListDraft{.name = "Unrelated"})));
    REQUIRE(fixture.executor().waitUntilQueued());

    auto submitCompletedPtr = std::make_shared<std::atomic_bool>(false);
    auto submitFuture = fixture.runtime().spawn(rt::test::flagCompletion(
      submitCompletedPtr, session.submitMetadata(rt::MetadataPatch{.optTitle = "Must not apply"})));
    REQUIRE(fixture.executor().waitUntilQueuedCount(2));

    REQUIRE(fixture.executor().runOne());
    CHECK(session.isCurrent());
    REQUIRE(fixture.executor().drainUntil([&] { return createCompletedPtr->load() && submitCompletedPtr->load(); }));

    auto createRes = createFuture.get();
    auto submitRes = submitFuture.get();
    REQUIRE(createRes);
    REQUIRE(submitRes);
    CHECK(submitRes->status == rt::AuthoringStatus::Busy);
    CHECK_FALSE(session.isCurrent());
    CHECK(invalidatedCount == 1);
    CHECK(fixture.title(fixture.trackIds().front()) == "Old Title");
  }

  TEST_CASE("TrackAuthoringSession - pending submission outlives moved and destroyed facades",
            "[uimodel][regression][library-authoring][concurrency]")
  {
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<TrackAuthoringSession>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<TrackAuthoringSession>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<TrackAuthoringSession>);

    auto fixture = TrackAuthoringFixture{1};
    std::size_t invalidatedCount = 0;
    auto invalidatedSubscription = async::Subscription{};
    auto createCompletedPtr = std::make_shared<std::atomic_bool>(false);
    auto submitCompletedPtr = std::make_shared<std::atomic_bool>(false);
    auto futures = [&]
    {
      auto source = ao::test::requireValue(TrackAuthoringSession::begin(fixture.library(), fixture.trackIds()));
      auto moved = std::move(source);
      invalidatedSubscription = moved.onInvalidated([&invalidatedCount] noexcept { ++invalidatedCount; });
      auto createFuture = fixture.runtime().spawn(rt::test::flagCompletion(
        createCompletedPtr, fixture.library().commands().createList(rt::ListDraft{.name = "Invalidate binding"})));
      REQUIRE(fixture.executor().waitUntilQueued());
      auto submitFuture = fixture.runtime().spawn(rt::test::flagCompletion(
        submitCompletedPtr, moved.submitMetadata(rt::MetadataPatch{.optTitle = "Must not apply"})));
      REQUIRE(fixture.executor().waitUntilQueuedCount(2));
      return std::pair{std::move(createFuture), std::move(submitFuture)};
    }();

    REQUIRE(fixture.executor().drainUntil([&] { return createCompletedPtr->load() && submitCompletedPtr->load(); }));
    REQUIRE(futures.first.get());
    auto const submitRes = futures.second.get();
    REQUIRE(submitRes);
    CHECK(submitRes->status == rt::AuthoringStatus::Busy);
    CHECK(invalidatedCount == 1);
    CHECK(fixture.title(fixture.trackIds().front()) == "Old Title");

    REQUIRE(fixture.runTask(fixture.library().commands().createList(rt::ListDraft{.name = "After cleanup"})));
    CHECK(invalidatedCount == 1);
  }
} // namespace ao::uimodel::test
