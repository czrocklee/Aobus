// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/PlaybackLaunchContext.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/projection/LiveTrackListProjection.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <iterator>
#include <memory>
#include <random>
#include <span>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    void checkProjectionMatches(LiveTrackListProjection const& actual, LiveTrackListProjection const& expected)
    {
      REQUIRE(actual.size() == expected.size());
      REQUIRE(actual.groupCount() == expected.groupCount());

      for (std::size_t row = 0; row < expected.size(); ++row)
      {
        auto const trackId = expected.trackIdAt(row);
        CHECK(actual.trackIdAt(row) == trackId);
        CHECK(actual.indexOf(trackId) == row);
        CHECK(actual.groupIndexAt(row) == expected.groupIndexAt(row));
      }

      for (std::size_t groupIndex = 0; groupIndex < expected.groupCount(); ++groupIndex)
      {
        auto const actualGroup = actual.groupAt(groupIndex);
        auto const expectedGroup = expected.groupAt(groupIndex);
        CHECK(actualGroup.rows.start == expectedGroup.rows.start);
        CHECK(actualGroup.rows.count == expectedGroup.rows.count);
        CHECK(actualGroup.primaryText == expectedGroup.primaryText);
        CHECK(actualGroup.secondaryText == expectedGroup.secondaryText);
        CHECK(actualGroup.tertiaryText == expectedGroup.tertiaryText);
        CHECK(actualGroup.imageId == expectedGroup.imageId);
      }
    }

    TrackPresentationSpec groupedPresentation()
    {
      return TrackPresentationSpec{
        .groupBy = TrackGroupKey::Artist,
        .sortBy =
          {
            TrackSortTerm{.field = TrackSortField::Artist},
            TrackSortTerm{.field = TrackSortField::Title},
          },
      };
    }
  } // namespace

  TEST_CASE("TrackListProjection - incremental batches match a fresh full rebuild",
            "[runtime][regression][projection][incremental]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto trackIds = std::vector<TrackId>{};

    for (std::size_t index = 0; index < 24; ++index)
    {
      trackIds.push_back(libraryFixture.addTrack(library::test::TrackSpec{
        .title = std::format("Track {:03}", index),
        .artist = std::format("Artist {}", index % 6),
        .genre = std::format("Genre {}", index % 4),
      }));
    }

    auto sourcePtr = makeMutableTrackSource(trackIds);
    auto const presentation = groupedPresentation();
    auto projection = LiveTrackListProjection{ViewId{1}, TrackSourceLease{sourcePtr}, libraryFixture.library()};
    projection.setPresentation(presentation);
    auto const before = projection.operationCounts();
    auto random = std::mt19937{0xA0B05U}; // NOLINT(bugprone-random-generator-seed) -- deterministic replay.
    constexpr std::size_t kIterations = 120;

    for (std::size_t iteration = 0; iteration < kIterations; ++iteration)
    {
      CAPTURE(iteration);

      switch (auto const currentIds = sourceTrackIds(*sourcePtr); iteration % 4U)
      {
        case 0:
        {
          auto const trackId = libraryFixture.addTrack(library::test::TrackSpec{
            .title = std::format("Inserted {:03}", iteration),
            .artist = std::format("Artist {}", iteration % 7U),
            .genre = std::format("Genre {}", iteration % 5U),
          });
          auto const position = static_cast<std::size_t>(random() % (currentIds.size() + 1U));
          sourcePtr->insert(trackId, position);
          break;
        }
        case 1:
        {
          REQUIRE(currentIds.size() > 8);
          auto const trackId = currentIds[static_cast<std::size_t>(random() % currentIds.size())];
          sourcePtr->remove(trackId);
          break;
        }
        case 2:
        {
          auto const trackId = currentIds[static_cast<std::size_t>(random() % currentIds.size())];
          libraryFixture.updateTrack(trackId,
                                     [iteration](library::test::TrackSpec& spec)
                                     {
                                       spec.title = std::format("Updated title {:03}", iteration);
                                       spec.artist = std::format("Updated artist {}", iteration % 5U);
                                     });
          sourcePtr->update(trackId);
          break;
        }
        default:
        {
          auto updatedIds = std::array{
            currentIds[static_cast<std::size_t>(random() % currentIds.size())],
            currentIds[static_cast<std::size_t>(random() % currentIds.size())],
            currentIds[static_cast<std::size_t>(random() % currentIds.size())],
          };
          std::ranges::sort(updatedIds);
          auto const duplicateRange = std::ranges::unique(updatedIds);
          auto const uniqueCount =
            static_cast<std::size_t>(std::ranges::distance(updatedIds.begin(), duplicateRange.begin()));
          auto const uniqueIds = std::span{updatedIds}.first(uniqueCount);

          for (auto const trackId : uniqueIds)
          {
            libraryFixture.updateTrack(trackId,
                                       [iteration, trackId](library::test::TrackSpec& spec)
                                       {
                                         spec.title = std::format("Batch {:03} track {}", iteration, trackId.raw());
                                         spec.artist = std::format("Batch artist {}", (iteration + trackId.raw()) % 8U);
                                       });
          }

          sourcePtr->batchUpdate(uniqueIds);
          break;
        }
      }

      auto oracle = LiveTrackListProjection{ViewId{2}, TrackSourceLease{sourcePtr}, libraryFixture.library()};
      oracle.setPresentation(presentation);
      checkProjectionMatches(projection, oracle);
    }

    auto const after = projection.operationCounts();
    CHECK(after.fullProjectionRebuilds == before.fullProjectionRebuilds);
    CHECK(after.incrementalProjectionUpdates - before.incrementalProjectionUpdates == kIterations);
    CHECK(after.arenaRebases == before.arenaRebases);
    CHECK(after.rowIndexRebuilds - before.rowIndexRebuilds == kIterations);
  }

  TEST_CASE("TrackListProjection - sustained metadata churn periodically rebases arena storage",
            "[runtime][unit][projection][incremental]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto trackIds = std::vector<TrackId>{};

    for (std::size_t index = 0; index < 8; ++index)
    {
      trackIds.push_back(
        libraryFixture.addTrack(library::test::TrackSpec{.title = std::format("Track {}", index), .artist = "Artist"}));
    }

    auto sourcePtr = makeMutableTrackSource(trackIds);
    auto projection = LiveTrackListProjection{
      kInvalidViewId,
      TrackSourceLease{sourcePtr},
      libraryFixture.library(),
      TrackOrderSpec{.sortBy = {TrackSortTerm{.field = TrackSortField::Title}}},
    };
    auto const before = projection.operationCounts();
    constexpr std::size_t kUpdateCount = 520;

    for (std::size_t iteration = 0; iteration < kUpdateCount; ++iteration)
    {
      libraryFixture.updateTrack(trackIds.front(),
                                 [iteration](library::test::TrackSpec& spec)
                                 { spec.title = std::format("Changing title {:04}", iteration); });
      sourcePtr->update(trackIds.front());
    }

    auto oracle = LiveTrackListProjection{
      kInvalidViewId,
      TrackSourceLease{sourcePtr},
      libraryFixture.library(),
      TrackOrderSpec{.sortBy = {TrackSortTerm{.field = TrackSortField::Title}}},
    };
    checkProjectionMatches(projection, oracle);

    auto const after = projection.operationCounts();
    CHECK(after.incrementalProjectionUpdates - before.incrementalProjectionUpdates == kUpdateCount);
    CHECK(after.arenaRebases - before.arenaRebases == 2);
    CHECK(after.fullProjectionRebuilds - before.fullProjectionRebuilds == 2);
    CHECK(after.rowIndexRebuilds - before.rowIndexRebuilds == kUpdateCount);
  }
} // namespace ao::rt::test
