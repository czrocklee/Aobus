// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    // Two distinct ad-hoc specs over All Tracks share one evaluator bucket.
    // The first-acquired source is registered first, so the evaluator's
    // publish loops reach it before the sibling's pending work item.
    std::optional<TrackSourceLease> acquireAdHocSibling(TrackSourceCache& cache,
                                                        ListId const baseListId,
                                                        std::string filterExpression)
    {
      return std::optional<TrackSourceLease>{ao::test::requireValue(
        cache.acquire(SourceSpec{.baseListId = baseListId, .filterExpression = std::move(filterExpression)}))};
    }

    std::optional<TrackSourceLease> acquireAdHocSibling(TrackSourceCache& cache, std::string filterExpression)
    {
      return acquireAdHocSibling(cache, kAllTracksListId, std::move(filterExpression));
    }
  } // namespace

  TEST_CASE("TrackSourceCache - a delta observer may release a sibling ad-hoc lease during an update-only batch",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack(library::test::TrackSpec{.title = "Alpha", .year = 2001});
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();

    auto optFirstLease = acquireAdHocSibling(cache, "$title = \"Gamma\"");
    auto optSecondLease = acquireAdHocSibling(cache, "true");
    REQUIRE(optFirstLease->source().size() == 0);
    REQUIRE(optSecondLease->source().size() == 1);

    auto firstBatches = std::vector<TrackSourceDelta>{};
    auto secondBatches = std::vector<TrackSourceDelta>{};
    auto optSecondSubscription = std::optional<async::Subscription>{};
    optSecondSubscription.emplace(optSecondLease->source().subscribe([&](TrackSourceDelta const& batch) noexcept
                                                                     { secondBatches.push_back(batch); }));
    auto firstSubscription = optFirstLease->source().subscribe(
      [&](TrackSourceDelta const& batch) noexcept
      {
        firstBatches.push_back(batch);

        // Source delivery is synchronous: releasing the sibling's last lease
        // runs its destructor while the evaluator is still publishing this
        // batch over the bucket's remaining work items.
        if (optSecondLease)
        {
          optSecondSubscription.reset();
          optSecondLease.reset();
        }
      });

    auto const updateRes = commandsFixture.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "Gamma"});
    REQUIRE(updateRes);

    REQUIRE(firstBatches.size() == 1);
    REQUIRE(sourceEditScript(firstBatches.front()).edits.size() == 1);
    auto const& insertion = std::get<delta::InsertRange>(sourceEditScript(firstBatches.front()).edits.front());
    CHECK(insertion.start == 0);
    CHECK(insertion.trackIds == std::vector{trackId});
    CHECK_FALSE(optSecondLease);
    CHECK(optFirstLease->source().state() == TrackSourceState::Live);
    CHECK(sourceTrackIds(optFirstLease->source()) == std::vector{trackId});
  }

  TEST_CASE("TrackSourceCache - a delta observer may release a sibling ad-hoc lease during a structural batch",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const keptId = libraryFixture.addTrack(library::test::TrackSpec{.title = "Alpha", .year = 2001});
    auto const removedId = libraryFixture.addTrack(library::test::TrackSpec{.title = "Beta", .year = 2002});
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();

    auto optFirstLease = acquireAdHocSibling(cache, "$year >= 2000");
    auto optSecondLease = acquireAdHocSibling(cache, "true");
    REQUIRE(optFirstLease->source().size() == 2);
    REQUIRE(optSecondLease->source().size() == 2);

    auto firstBatches = std::vector<TrackSourceDelta>{};
    auto secondBatches = std::vector<TrackSourceDelta>{};
    auto optSecondSubscription = std::optional<async::Subscription>{};
    optSecondSubscription.emplace(optSecondLease->source().subscribe([&](TrackSourceDelta const& batch) noexcept
                                                                     { secondBatches.push_back(batch); }));
    auto firstSubscription = optFirstLease->source().subscribe(
      [&](TrackSourceDelta const& batch) noexcept
      {
        firstBatches.push_back(batch);

        if (optSecondLease)
        {
          optSecondSubscription.reset();
          optSecondLease.reset();
        }
      });

    REQUIRE(commandsFixture.runTask(commandsFixture.commands().deleteTrackAsync(removedId)));

    REQUIRE(firstBatches.size() == 1);
    REQUIRE(sourceEditScript(firstBatches.front()).edits.size() == 1);
    auto const& removal = std::get<delta::RemoveRange>(sourceEditScript(firstBatches.front()).edits.front());
    CHECK(removal.trackIds == std::vector{removedId});
    CHECK_FALSE(optSecondLease);
    CHECK(optFirstLease->source().state() == TrackSourceState::Live);
    CHECK(sourceTrackIds(optFirstLease->source()) == std::vector{keptId});
  }

  TEST_CASE("TrackSourceCache - a delta observer may release a sibling ad-hoc lease during an upstream reset",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack(library::test::TrackSpec{.title = "Alpha", .year = 2001});
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();

    auto optFirstLease = acquireAdHocSibling(cache, "$year >= 2000");
    auto optSecondLease = acquireAdHocSibling(cache, "true");
    REQUIRE(optFirstLease->source().size() == 1);
    REQUIRE(optSecondLease->source().size() == 1);

    auto firstBatches = std::vector<TrackSourceDelta>{};
    auto secondBatches = std::vector<TrackSourceDelta>{};
    auto optSecondSubscription = std::optional<async::Subscription>{};
    optSecondSubscription.emplace(optSecondLease->source().subscribe([&](TrackSourceDelta const& batch) noexcept
                                                                     { secondBatches.push_back(batch); }));
    auto firstSubscription = optFirstLease->source().subscribe(
      [&](TrackSourceDelta const& batch) noexcept
      {
        firstBatches.push_back(batch);

        if (optSecondLease)
        {
          optSecondSubscription.reset();
          optSecondLease.reset();
        }
      });

    // reloadAllTracks publishes one SourceReset on All Tracks; the evaluator
    // walks the live bucket's list while publishing each member's reset.
    cache.reloadAllTracks();

    REQUIRE(firstBatches.size() == 1);
    CHECK(std::holds_alternative<SourceReset>(firstBatches.front()));
    CHECK_FALSE(optSecondLease);
    CHECK(optFirstLease->source().state() == TrackSourceState::Live);
    CHECK(sourceTrackIds(optFirstLease->source()) == std::vector{trackId});
  }

  TEST_CASE(
    "TrackSourceCache - a delta observer may release the publishing source's own lease during a structural batch",
    "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const keptId = libraryFixture.addTrack(library::test::TrackSpec{.title = "Alpha", .year = 2001});
    auto const removedId = libraryFixture.addTrack(library::test::TrackSpec{.title = "Beta", .year = 2002});
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();

    auto optFirstLease = acquireAdHocSibling(cache, "$year >= 2000");
    auto optSecondLease = acquireAdHocSibling(cache, "true");

    auto firstBatches = std::vector<TrackSourceDelta>{};
    auto secondBatches = std::vector<TrackSourceDelta>{};
    auto optSecondSubscription = std::optional<async::Subscription>{};
    optSecondSubscription.emplace(optSecondLease->source().subscribe([&](TrackSourceDelta const& batch) noexcept
                                                                     { secondBatches.push_back(batch); }));
    auto firstSubscription = optFirstLease->source().subscribe(
      [&](TrackSourceDelta const& batch) noexcept
      {
        firstBatches.push_back(batch);

        // The publisher drops its own last lease mid-publication; the sibling
        // must still receive exactly its own batch afterwards.
        if (optFirstLease)
        {
          optFirstLease.reset();
        }
      });

    REQUIRE(commandsFixture.runTask(commandsFixture.commands().deleteTrackAsync(removedId)));

    REQUIRE(firstBatches.size() == 1);
    REQUIRE(sourceEditScript(firstBatches.front()).edits.size() == 1);
    auto const& ownRemoval = std::get<delta::RemoveRange>(sourceEditScript(firstBatches.front()).edits.front());
    CHECK(ownRemoval.trackIds == std::vector{removedId});
    CHECK_FALSE(optFirstLease);
    REQUIRE(secondBatches.size() == 1);
    REQUIRE(sourceEditScript(secondBatches.front()).edits.size() == 1);
    auto const& siblingRemoval = std::get<delta::RemoveRange>(sourceEditScript(secondBatches.front()).edits.front());
    CHECK(siblingRemoval.trackIds == std::vector{removedId});
    CHECK(optSecondLease->source().state() == TrackSourceState::Live);
    CHECK(sourceTrackIds(optSecondLease->source()) == std::vector{keptId});
  }

  TEST_CASE(
    "TrackSourceCache - a delta observer may release the publishing source's own lease during an upstream reset",
    "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack(library::test::TrackSpec{.title = "Alpha", .year = 2001});
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();

    auto optFirstLease = acquireAdHocSibling(cache, "$year >= 2000");
    auto optSecondLease = acquireAdHocSibling(cache, "true");
    CHECK(sourceTrackIds(optSecondLease->source()) == std::vector{trackId});

    auto firstBatches = std::vector<TrackSourceDelta>{};
    auto secondBatches = std::vector<TrackSourceDelta>{};
    auto optSecondSubscription = std::optional<async::Subscription>{};
    optSecondSubscription.emplace(optSecondLease->source().subscribe([&](TrackSourceDelta const& batch) noexcept
                                                                     { secondBatches.push_back(batch); }));
    auto firstSubscription = optFirstLease->source().subscribe(
      [&](TrackSourceDelta const& batch) noexcept
      {
        firstBatches.push_back(batch);

        // Retiring the whole bucket mid-publication must not free the bucket
        // while the evaluator still walks it.
        if (optFirstLease)
        {
          optSecondSubscription.reset();
          optSecondLease.reset();
          optFirstLease.reset();
        }
      });

    cache.reloadAllTracks();

    REQUIRE(firstBatches.size() == 1);
    CHECK(std::holds_alternative<SourceReset>(firstBatches.front()));
    CHECK_FALSE(optFirstLease);
    CHECK_FALSE(optSecondLease);
    CHECK(secondBatches.empty());
  }

  TEST_CASE("TrackSourceCache - a delta observer may replace a retired bucket with a new source during a reset",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack(library::test::TrackSpec{.title = "Alpha", .year = 2001});
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();

    auto optFirstLease = acquireAdHocSibling(cache, "$year >= 2000");
    auto optSecondLease = acquireAdHocSibling(cache, "true");
    CHECK(sourceTrackIds(optSecondLease->source()) == std::vector{trackId});

    auto firstBatches = std::vector<TrackSourceDelta>{};
    auto thirdBatches = std::vector<TrackSourceDelta>{};
    auto optThirdLease = std::optional<TrackSourceLease>{};
    auto optThirdSubscription = std::optional<async::Subscription>{};
    auto firstSubscription = optFirstLease->source().subscribe(
      [&](TrackSourceDelta const& batch) noexcept
      {
        firstBatches.push_back(batch);

        // Retire the whole bucket, then register a fresh source on the same
        // upstream while the publication is still on the stack. The bucket
        // must be rebuilt coherently instead of reusing the retired mirror.
        if (optFirstLease)
        {
          optSecondLease.reset();
          optFirstLease.reset();
          optThirdLease = acquireAdHocSibling(cache, "$title ~ \"Al\"");
          optThirdSubscription.emplace(optThirdLease->source().subscribe([&](TrackSourceDelta const& batch) noexcept
                                                                         { thirdBatches.push_back(batch); }));
        }
      });

    cache.reloadAllTracks();

    REQUIRE(firstBatches.size() == 1);
    CHECK(std::holds_alternative<SourceReset>(firstBatches.front()));
    CHECK_FALSE(optFirstLease);
    CHECK_FALSE(optSecondLease);
    REQUIRE(optThirdLease);
    CHECK(optThirdLease->source().state() == TrackSourceState::Live);
    CHECK(sourceTrackIds(optThirdLease->source()) == std::vector{trackId});
    CHECK(thirdBatches.empty());
  }

  TEST_CASE(
    "TrackSourceCache - an invalidation observer may release a sibling ad-hoc lease during upstream invalidation",
    "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack(library::test::TrackSpec{.title = "Alpha", .year = 2001});
    auto listId = ListId{0};
    {
      auto transaction = library::test::writeTransaction(libraryFixture.library());
      auto builder = library::ListBuilder::makeEmpty().name("Invalidated base");
      listId = ao::test::requireValue(
        transaction.apply([&builder](library::LibraryWrite& write) { return write.lists().create(builder); }));
      REQUIRE(transaction.commit());
    }

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();

    auto optFirstLease = acquireAdHocSibling(cache, listId, "$year >= 2000");
    auto optSecondLease = acquireAdHocSibling(cache, listId, "true");
    REQUIRE(optFirstLease->source().size() == 1);
    CHECK(sourceTrackIds(optSecondLease->source()) == std::vector{trackId});

    auto firstBatches = std::vector<TrackSourceDelta>{};
    auto secondBatches = std::vector<TrackSourceDelta>{};
    auto optSecondSubscription = std::optional<async::Subscription>{};
    optSecondSubscription.emplace(optSecondLease->source().subscribe([&](TrackSourceDelta const& batch) noexcept
                                                                     { secondBatches.push_back(batch); }));
    auto firstSubscription = optFirstLease->source().subscribe(
      [&](TrackSourceDelta const& batch) noexcept
      {
        firstBatches.push_back(batch);

        // Deleting the base list invalidates both dependents; releasing the
        // sibling during that delivery must not corrupt the invalidation walk.
        if (optSecondLease)
        {
          optSecondSubscription.reset();
          optSecondLease.reset();
        }
      });

    REQUIRE(commandsFixture.runTask(commandsFixture.commands().deleteListAsync(listId)));

    REQUIRE(firstBatches.size() == 1);
    CHECK(std::holds_alternative<SourceInvalidated>(firstBatches.front()));
    CHECK(optFirstLease->source().state() == TrackSourceState::Invalidated);
    CHECK(optFirstLease->source().size() == 0);
    CHECK_FALSE(optSecondLease);
    CHECK(secondBatches.empty());

    auto const reacquireRes = cache.acquire(listId);
    REQUIRE_FALSE(reacquireRes);
    CHECK(reacquireRes.error().code == Error::Code::NotFound);
  }

  TEST_CASE("TrackSourceCache - a delta observer acquiring a new ad-hoc source during publication leaves it coherent",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const firstId = libraryFixture.addTrack(library::test::TrackSpec{.title = "Alpha", .year = 2001});
    auto const secondId = libraryFixture.addTrack(library::test::TrackSpec{.title = "Beta", .year = 2002});
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();

    auto optFirstLease = acquireAdHocSibling(cache, "$title = \"Gamma\"");
    auto optSecondLease = acquireAdHocSibling(cache, "true");

    auto firstBatches = std::vector<TrackSourceDelta>{};
    auto secondBatches = std::vector<TrackSourceDelta>{};
    auto thirdBatches = std::vector<TrackSourceDelta>{};
    auto optThirdLease = std::optional<TrackSourceLease>{};
    auto optThirdSubscription = std::optional<async::Subscription>{};
    [[maybe_unused]] auto secondSubscription = optSecondLease->source().subscribe(
      [&](TrackSourceDelta const& batch) noexcept { secondBatches.push_back(batch); });
    auto firstSubscription = optFirstLease->source().subscribe(
      [&](TrackSourceDelta const& batch) noexcept
      {
        firstBatches.push_back(batch);

        // A subscriber acquiring a new ad-hoc source mid-publication must
        // leave it with a coherent snapshot and no stale batch from the
        // publication already in flight.
        if (!optThirdLease)
        {
          optThirdLease = acquireAdHocSibling(cache, "$year >= 2000");
          optThirdSubscription.emplace(optThirdLease->source().subscribe([&](TrackSourceDelta const& batch) noexcept
                                                                         { thirdBatches.push_back(batch); }));
        }
      });

    auto const updateRes = commandsFixture.updateMetadata(std::array{firstId}, MetadataPatch{.optTitle = "Gamma"});
    REQUIRE(updateRes);

    REQUIRE(firstBatches.size() == 1);
    REQUIRE(sourceEditScript(firstBatches.front()).edits.size() == 1);
    auto const& insertion = std::get<delta::InsertRange>(sourceEditScript(firstBatches.front()).edits.front());
    CHECK(insertion.start == 0);
    CHECK(insertion.trackIds == std::vector{firstId});
    REQUIRE(secondBatches.size() == 1);
    REQUIRE(sourceEditScript(secondBatches.front()).edits.size() == 1);
    auto const& update = std::get<delta::UpdateRange>(sourceEditScript(secondBatches.front()).edits.front());
    CHECK(update.trackIds == std::vector{firstId});
    REQUIRE(optThirdLease);
    CHECK(thirdBatches.empty());
    CHECK(optThirdLease->source().state() == TrackSourceState::Live);
    CHECK(sourceTrackIds(optThirdLease->source()) == std::vector{firstId, secondId});
  }
} // namespace ao::rt::test
