// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/projection/ProjectionTypes.h>
#include <ao/rt/projection/TrackListProjection.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("TrackListProjection - 10k scale operations preserve indices and deltas",
            "[runtime][unit][projection][scale]")
  {
    auto env = TestMusicLibrary{};
    auto& lib = env.library();

    int const kTrackCount = 10000;
    auto ids = std::vector<TrackId>{};
    ids.reserve(kTrackCount);

    for (std::int32_t idx = 0; idx < kTrackCount; ++idx)
    {
      ids.push_back(env.addTrack(library::test::TrackSpec{.title = std::format("Track {:05d}", idx),
                                                          .artist = std::format("Artist {:03d}", idx % 100),
                                                          .album = std::format("Album {:03d}", idx % 500)}));
    }

    auto source = MutableTrackSource{};
    source.setInitial(ids);

    auto proj = TrackListProjection{ViewId{1}, source, lib};
    proj.setPresentation(
      TrackPresentationSpec{.groupBy = TrackGroupKey::None, .sortBy = {TrackSortTerm{.field = TrackSortField::Title}}});

    CHECK(proj.size() == kTrackCount);

    SECTION("Batch insertion performance (incremental merge)")
    {
      auto newIds = std::vector<TrackId>{};
      newIds.reserve(100);

      for (std::int32_t idx = 0; idx < 100; ++idx)
      {
        newIds.push_back(env.addTrack(library::test::TrackSpec{.title = std::format("New Track {:05d}", idx)}));
      }

      source.batchInsert(newIds);

      CHECK(proj.size() == kTrackCount + 100);

      for (auto const id : newIds)
      {
        auto const optIndex = proj.indexOf(id);
        REQUIRE(optIndex);
        CHECK(proj.trackIdAt(*optIndex) == id);
      }
    }

    SECTION("indexOf returns stable positions across the full list")
    {
      auto const firstId = proj.trackIdAt(0);
      auto const midId = proj.trackIdAt(static_cast<std::size_t>(kTrackCount / 2));
      auto const lastId = proj.trackIdAt(static_cast<std::size_t>(kTrackCount - 1));

      for (std::int32_t i = 0; i < 10; ++i)
      {
        CHECK(proj.indexOf(firstId) == std::size_t{0});
        CHECK(proj.indexOf(midId) == static_cast<std::size_t>(kTrackCount / 2));
        CHECK(proj.indexOf(lastId) == static_cast<std::size_t>(kTrackCount - 1));
      }

      CHECK_FALSE(proj.indexOf(TrackId{999999}).has_value());
    }

    SECTION("reset delta keeps projected size stable")
    {
      auto const projSize = proj.size();
      bool receivedReset = false;
      std::size_t receivedSize = 0;

      auto const sub = proj.subscribe(
        [&](TrackListProjectionDeltaBatch const& batch)
        {
          if (batch.deltas.empty())
          {
            return;
          }

          if (std::holds_alternative<ProjectionReset>(batch.deltas.front()))
          {
            receivedReset = true;
            receivedSize = proj.size();
          }
        });

      proj.setPresentation(TrackPresentationSpec{
        .groupBy = TrackGroupKey::Artist, .sortBy = {TrackSortTerm{.field = TrackSortField::Artist}}});

      CHECK(receivedReset);
      CHECK(receivedSize == projSize);
    }
  }
} // namespace ao::rt::test
