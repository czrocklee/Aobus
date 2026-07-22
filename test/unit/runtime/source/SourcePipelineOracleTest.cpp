// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/ListStore.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/PlaybackLaunchSpec.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/projection/LiveTrackListProjection.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

    std::vector<TrackId> storedManualTrackIds(library::MusicLibrary& library, ListId listId)
    {
      auto transaction = library.readTransaction();
      auto optView = library.lists().reader(transaction).get(listId);
      REQUIRE(optView);
      return {optView->tracks().begin(), optView->tracks().end()};
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

        if (library::test::trackSpecFromView(library, *optView).year >= minimumYear)
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
        auto const optView = reader.get(trackId, library::TrackStore::Reader::LoadMode::Both);
        REQUIRE(optView);
        titles.emplace_back(trackId, library::test::trackSpecFromView(library, *optView).title);
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

    auto changes = LibraryChanges{};
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const manualListId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .name = "Oracle manual",
      .trackIds = initialTrackIds,
    }));
    auto const smartListId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Smart,
      .name = "Oracle smart",
      .expression = "$year >= 2020",
    }));

    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();
    auto allTracksLease = ao::test::requireValue(cache.acquire(kAllTracksListId));
    auto manualLease = ao::test::requireValue(cache.acquire(manualListId));
    auto smartLease = ao::test::requireValue(cache.acquire(smartListId));
    auto manualProjection = LiveTrackListProjection{
      kInvalidViewId,
      manualLease,
      libraryFixture.library(),
      TrackOrderSpec{.sortBy = {TrackSortTerm{.field = TrackSortField::Title}}},
    };
    auto smartProjection = LiveTrackListProjection{
      kInvalidViewId,
      smartLease,
      libraryFixture.library(),
      TrackOrderSpec{.sortBy = {TrackSortTerm{.field = TrackSortField::Title}}},
    };

    auto assertOracle = [&]
    {
      auto const allExpected = storedTrackIds(libraryFixture.library());
      auto const manualExpected = storedManualTrackIds(libraryFixture.library(), manualListId);
      auto const smartExpected = matchingYears(libraryFixture.library(), allExpected, 2020);

      CHECK(sourceTrackIds(allTracksLease.source()) == allExpected);
      CHECK(sourceTrackIds(manualLease.source()) == manualExpected);
      CHECK(sourceTrackIds(smartLease.source()) == smartExpected);
      CHECK(projectionTrackIds(manualProjection) == sortedByTitle(libraryFixture.library(), manualExpected));
      CHECK(projectionTrackIds(smartProjection) == sortedByTitle(libraryFixture.library(), smartExpected));
    };

    assertOracle();

    for (std::uint32_t step = 0; step < 64; ++step)
    {
      auto const liveTrackIds = storedTrackIds(libraryFixture.library());
      auto const manualTrackIds = storedManualTrackIds(libraryFixture.library(), manualListId);
      REQUIRE_FALSE(liveTrackIds.empty());

      switch (step % 5U)
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
          if (manualTrackIds.size() > 2)
          {
            auto const target = manualTrackIds[step % manualTrackIds.size()];
            REQUIRE(writer.moveManualListTracks(manualListId, std::span{&target, 1}, 0));
          }

          break;
        case 2:
          if (manualTrackIds.size() > 8)
          {
            auto const target = manualTrackIds[step % manualTrackIds.size()];
            REQUIRE(writer.removeManualListTracks(manualListId, std::span{&target, 1}));
          }

          break;
        case 3:
          if (liveTrackIds.size() > 12)
          {
            REQUIRE(writer.deleteTrack(liveTrackIds.back()));
          }

          break;
        case 4:
        {
          auto const candidate = std::ranges::find_if(liveTrackIds,
                                                      [&manualTrackIds](TrackId trackId)
                                                      { return !std::ranges::contains(manualTrackIds, trackId); });

          if (candidate != liveTrackIds.end())
          {
            REQUIRE(writer.insertManualListTracks(manualListId, manualTrackIds.size(), std::span{&*candidate, 1}));
          }

          break;
        }
        default: break;
      }

      assertOracle();
    }
  }
} // namespace ao::rt::test
