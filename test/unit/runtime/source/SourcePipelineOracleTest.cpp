// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/ListStore.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/PlaybackLaunchSpec.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    std::vector<TrackId> storedTrackIds(library::MusicLibrary& library)
    {
      auto transaction = library.readTransaction();
      auto reader = library.tracks().reader(transaction);
      auto result = std::vector<TrackId>{};

      for (auto const [trackId, view] : reader.hot())
      {
        std::ignore = view;
        result.push_back(trackId);
      }

      return result;
    }

    std::vector<TrackId> storedOrderTrackIds(library::MusicLibrary& library, ListId listId)
    {
      auto transaction = library.readTransaction();
      auto optView = library.lists().reader(transaction).get(listId);
      REQUIRE(optView);
      return {optView->orderTrackIds().begin(), optView->orderTrackIds().end()};
    }

    std::vector<TrackId> effectiveOrder(std::span<TrackId const> orderTrackIds, std::span<TrackId const> parentTrackIds)
    {
      auto result = std::vector<TrackId>{};
      result.reserve(parentTrackIds.size());

      for (auto const trackId : orderTrackIds)
      {
        if (std::ranges::contains(parentTrackIds, trackId))
        {
          result.push_back(trackId);
        }
      }

      for (auto const trackId : parentTrackIds)
      {
        if (!std::ranges::contains(orderTrackIds, trackId))
        {
          result.push_back(trackId);
        }
      }

      return result;
    }

    std::vector<TrackId> matchingYears(library::MusicLibrary& library,
                                       std::span<TrackId const> trackIds,
                                       std::uint16_t minimumYear)
    {
      auto transaction = library.readTransaction();
      auto reader = library.tracks().reader(transaction);
      auto result = std::vector<TrackId>{};

      for (auto const trackId : trackIds)
      {
        auto const optView = reader.get(trackId, library::TrackStore::Reader::LoadMode::Hot);
        REQUIRE(optView);

        if (optView->metadata().year() >= minimumYear)
        {
          result.push_back(trackId);
        }
      }

      return result;
    }

    std::vector<TrackId> sortedByTitle(library::MusicLibrary& library, std::span<TrackId const> trackIds)
    {
      auto titles = std::vector<std::pair<TrackId, std::string>>{};
      titles.reserve(trackIds.size());
      auto transaction = library.readTransaction();
      auto reader = library.tracks().reader(transaction);

      for (auto const trackId : trackIds)
      {
        auto const optView = reader.get(trackId, library::TrackStore::Reader::LoadMode::Hot);
        REQUIRE(optView);
        titles.emplace_back(trackId, optView->metadata().title());
      }

      std::ranges::stable_sort(titles, {}, &std::pair<TrackId, std::string>::second);
      return titles | std::views::keys | std::ranges::to<std::vector>();
    }

    std::vector<TrackId> projectionTrackIds(TrackListProjection const& projection)
    {
      auto result = std::vector<TrackId>{};
      result.reserve(projection.size());

      for (std::size_t index = 0; index < projection.size(); ++index)
      {
        result.push_back(projection.trackIdAt(index));
      }

      return result;
    }
  } // namespace

  TEST_CASE("Source pipeline oracle - mutation storm matches full recomputation after every write",
            "[runtime][unit][source][oracle]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto initialTrackIds = std::vector<TrackId>{};

    for (std::uint32_t index = 0; index < 24; ++index)
    {
      initialTrackIds.push_back(libraryFixture.addTrack(library::test::TrackSpec{
        .title = std::format("Track {:03}", index),
        .artist = std::format("Artist {:02}", index % 4U),
        .year = static_cast<std::uint16_t>(2010U + (index % 20U)),
      }));
    }

    auto changes = makeInlineLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const orderedListId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .name = "Oracle ordered",
      .expression = "$year >= 2020",
    }));
    auto const smartListId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .name = "Oracle smart",
      .expression = "$year >= 2020",
    }));

    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();
    auto allTracksLease = ao::test::requireValue(cache.acquire(kAllTracksListId));
    auto orderedLease = ao::test::requireValue(cache.acquire(orderedListId));
    auto smartLease = ao::test::requireValue(cache.acquire(smartListId));
    auto orderedProjection = TrackListProjection{
      kInvalidViewId,
      orderedLease,
      libraryFixture.library(),
      TrackOrderSpec{.sortBy = {TrackSortTerm{.field = TrackSortField::Title}}},
    };
    auto smartProjection = TrackListProjection{
      kInvalidViewId,
      smartLease,
      libraryFixture.library(),
      TrackOrderSpec{.sortBy = {TrackSortTerm{.field = TrackSortField::Title}}},
    };

    auto assertOracle = [&]
    {
      auto const allExpected = storedTrackIds(libraryFixture.library());
      auto const orderedParentExpected = matchingYears(libraryFixture.library(), allExpected, 2020);
      auto const orderedExpected =
        effectiveOrder(storedOrderTrackIds(libraryFixture.library(), orderedListId), orderedParentExpected);
      auto const smartExpected = matchingYears(libraryFixture.library(), allExpected, 2020);

      CHECK(sourceTrackIds(allTracksLease.source()) == allExpected);
      CHECK(sourceTrackIds(orderedLease.source()) == orderedExpected);
      CHECK(sourceTrackIds(smartLease.source()) == smartExpected);
      CHECK(projectionTrackIds(orderedProjection) == sortedByTitle(libraryFixture.library(), orderedExpected));
      CHECK(projectionTrackIds(smartProjection) == sortedByTitle(libraryFixture.library(), smartExpected));
    };

    assertOracle();

    {
      auto const visibleTrackIds = sourceTrackIds(orderedLease.source());
      REQUIRE(visibleTrackIds.size() > 1);
      auto const rankedTrackId = visibleTrackIds.back();
      auto binding = ao::test::requireValue(writerFixture.library().bindListOrder(orderedListId, visibleTrackIds));
      auto const move = writer.moveListOrder(binding, std::array{rankedTrackId}, visibleTrackIds.front());
      REQUIRE(move);
      REQUIRE(move->status == ListOrderAuthoringStatus::Applied);
      assertOracle();

      REQUIRE(writerFixture.updateMetadata(std::array{rankedTrackId}, MetadataPatch{.optYear = 2010}));
      CHECK_FALSE(orderedLease->indexOf(rankedTrackId).has_value());
      assertOracle();

      REQUIRE(writerFixture.updateMetadata(std::array{rankedTrackId}, MetadataPatch{.optYear = 2025}));
      REQUIRE(orderedLease->indexOf(rankedTrackId).has_value());
      CHECK(orderedLease->trackIdAt(0) == rankedTrackId);
      assertOracle();
    }

    for (std::uint32_t step = 0; step < 48; ++step)
    {
      auto const liveTrackIds = storedTrackIds(libraryFixture.library());
      auto const orderedTrackIds = sourceTrackIds(orderedLease.source());
      REQUIRE_FALSE(liveTrackIds.empty());

      switch (step % 4U)
      {
        case 0:
        {
          auto const target = liveTrackIds[step % liveTrackIds.size()];
          REQUIRE(
            writerFixture.updateMetadata(std::span{&target, 1},
                                         MetadataPatch{.optTitle = std::format("Mutation {:03}", step),
                                                       .optYear = static_cast<std::uint16_t>(2015U + (step % 15U))}));
          break;
        }
        case 1:
          if (orderedTrackIds.size() > 2)
          {
            auto const target = orderedTrackIds[1 + (step % (orderedTrackIds.size() - 1))];
            auto binding =
              ao::test::requireValue(writerFixture.library().bindListOrder(orderedListId, orderedTrackIds));
            auto const result = writer.moveListOrder(binding, std::span{&target, 1}, orderedTrackIds.front());
            REQUIRE(result);
            REQUIRE((result->status == ListOrderAuthoringStatus::Applied ||
                     result->status == ListOrderAuthoringStatus::NoOp));
          }

          break;
        case 2:
        {
          auto binding = ao::test::requireValue(writerFixture.library().bindListOrder(orderedListId, orderedTrackIds));
          auto const result = writer.resetListOrder(binding);
          REQUIRE(result);
          REQUIRE(
            (result->status == ListOrderAuthoringStatus::Applied || result->status == ListOrderAuthoringStatus::NoOp));

          break;
        }
        case 3:
          if (liveTrackIds.size() > 12)
          {
            REQUIRE(writer.deleteTrack(liveTrackIds.back()));
          }

          break;
        default: break;
      }

      assertOracle();
    }
  }
} // namespace ao::rt::test
