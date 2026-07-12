// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/PlaybackLaunchSpec.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/projection/LiveTrackListProjection.h>
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("Source pipeline - one source batch incrementally updates each projection",
            "[runtime][unit][source][operation-count]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto trackIds = std::vector<TrackId>{};
    trackIds.reserve(96);

    for (std::size_t index = 0; index < 96; ++index)
    {
      trackIds.push_back(libraryFixture.addTrack(library::test::TrackSpec{
        .title = std::format("Track {:03}", index),
        .artist = std::format("Artist {:02}", index % 8),
        .year = static_cast<std::uint16_t>(2000 + (index % 24)),
      }));
    }

    auto sourcePtr = makeMutableTrackSource(trackIds);
    auto evaluator = SmartListEvaluator{libraryFixture.library()};
    auto smartSources = std::array<std::shared_ptr<SmartListSource>, 3>{};
    auto projections = std::vector<std::unique_ptr<LiveTrackListProjection>>{};
    projections.reserve(smartSources.size());

    for (auto& smartSourcePtr : smartSources)
    {
      smartSourcePtr =
        std::make_shared<SmartListSource>(TrackSourceLease{sourcePtr}, libraryFixture.library(), evaluator);
      smartSourcePtr->reload();
      projections.push_back(std::make_unique<LiveTrackListProjection>(
        kInvalidViewId,
        TrackSourceLease{smartSourcePtr},
        libraryFixture.library(),
        TrackOrderSpec{.sortBy = {TrackSortTerm{.field = TrackSortField::Title}}}));
    }

    auto const before = projections.front()->operationCounts();
    auto const evaluatorBefore = evaluator.operationCounts();
    CHECK(before.fullProjectionRebuilds == 1);
    CHECK(before.incrementalProjectionUpdates == 0);
    CHECK(before.arenaRebases == 0);
    CHECK(before.rowIndexRebuilds == 1);

    auto const updatedIds = std::array{trackIds[2], trackIds[35], trackIds[70]};

    for (auto const trackId : updatedIds)
    {
      libraryFixture.updateTrack(trackId, [](library::test::TrackSpec& spec) { spec.title += " updated"; });
    }

    sourcePtr->batchUpdate(updatedIds);

    auto const evaluatorAfter = evaluator.operationCounts();
    CHECK(evaluatorAfter.upstreamIndexRebuilds - evaluatorBefore.upstreamIndexRebuilds == 1);
    CHECK(evaluatorAfter.membershipIndexRebuilds - evaluatorBefore.membershipIndexRebuilds == smartSources.size());

    for (auto const& projectionPtr : projections)
    {
      CHECK(projectionPtr->size() == trackIds.size());

      for (auto const trackId : updatedIds)
      {
        CHECK(projectionPtr->indexOf(trackId).has_value());
      }

      auto const after = projectionPtr->operationCounts();
      CHECK(after.fullProjectionRebuilds == before.fullProjectionRebuilds);
      CHECK(after.incrementalProjectionUpdates - before.incrementalProjectionUpdates == 1);
      CHECK(after.arenaRebases == before.arenaRebases);
      CHECK(after.rowIndexRebuilds - before.rowIndexRebuilds == 1);
    }
  }
} // namespace ao::rt::test
