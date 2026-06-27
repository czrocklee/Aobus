// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/runtime/TrackSourceTestSupport.h"
#include <ao/Type.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/projection/ProjectionTypes.h>
#include <ao/rt/projection/TrackListProjection.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("TrackListProjection - 10k Scale Performance", "[app][unit][runtime][projection][scale]")
  {
    auto env = TestMusicLibrary{};
    auto& lib = env.library();

    int const kTrackCount = 10000;
    auto ids = std::vector<TrackId>{};
    ids.reserve(kTrackCount);

    for (std::int32_t idx = 0; idx < kTrackCount; ++idx)
    {
      ids.push_back(env.addTrack(TrackSpec{.title = std::format("Track {:05d}", idx),
                                           .artist = std::format("Artist {:03d}", idx % 100),
                                           .album = std::format("Album {:03d}", idx % 500)}));
    }

    auto source = MutableTrackSource{};
    source.setInitial(ids);

    auto const start = std::chrono::steady_clock::now();
    auto proj = TrackListProjection{ViewId{1}, source, lib};
    proj.setPresentation(
      TrackPresentationSpec{.groupBy = TrackGroupKey::None, .sortBy = {TrackSortTerm{.field = TrackSortField::Title}}});
    auto const end = std::chrono::steady_clock::now();

    auto const duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    CHECK(proj.size() == kTrackCount);
    // Initial 10k sort + normalization should be reasonably fast with our cache.
    // We expect it to be well under 500ms on a typical dev machine.
    CHECK(duration < std::chrono::seconds{1});

    SECTION("Batch insertion performance (incremental merge)")
    {
      auto newIds = std::vector<TrackId>{};
      newIds.reserve(100);

      for (std::int32_t idx = 0; idx < 100; ++idx)
      {
        newIds.push_back(env.addTrack(TrackSpec{.title = std::format("New Track {:05d}", idx)}));
      }

      auto const batchStart = std::chrono::steady_clock::now();
      // Trigger batch insertion via source
      source.batchInsert(newIds);
      auto const batchEnd = std::chrono::steady_clock::now();

      auto const batchDuration = std::chrono::duration_cast<std::chrono::milliseconds>(batchEnd - batchStart);

      CHECK(proj.size() == kTrackCount + 100);
      // Merging 100 into 10k should be very fast (< 50ms)
      CHECK(batchDuration < std::chrono::milliseconds{100});
    }

    SECTION("indexOf O(1) lookup performance")
    {
      // Verify indexOf is constant-time regardless of lookup position
      auto const firstId = proj.trackIdAt(0);
      auto const midId = proj.trackIdAt(static_cast<std::size_t>(kTrackCount / 2));
      auto const lastId = proj.trackIdAt(static_cast<std::size_t>(kTrackCount - 1));

      // Warm up
      [[maybe_unused]] auto const optWarmup = proj.indexOf(firstId);

      constexpr int kIterations = 10000;
      auto const measureIndexOf = [&](TrackId id)
      {
        auto const t0 = std::chrono::steady_clock::now();

        for (std::int32_t i = 0; i < kIterations; ++i)
        {
          [[maybe_unused]] auto const optResult = proj.indexOf(id);
        }

        auto const t1 = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0);
      };

      auto const firstDuration = measureIndexOf(firstId);
      auto const midDuration = measureIndexOf(midId);
      auto const lastDuration = measureIndexOf(lastId);

      // All lookups should be fast regardless of position
      CHECK(firstDuration < std::chrono::microseconds{5000});
      CHECK(midDuration < std::chrono::microseconds{5000});
      CHECK(lastDuration < std::chrono::microseconds{5000});

      // O(1) property: mid and last should be within 2x of first
      auto const baselineDuration = std::max(std::chrono::microseconds{1}, firstDuration);
      auto const ratioMid = static_cast<double>(midDuration.count()) / static_cast<double>(baselineDuration.count());
      auto const ratioLast = static_cast<double>(lastDuration.count()) / static_cast<double>(baselineDuration.count());
      CHECK(ratioMid < 2.0);
      CHECK(ratioLast < 2.0);
    }

    SECTION("Reset delta application latency")
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

      auto const resetStart = std::chrono::steady_clock::now();
      proj.setPresentation(TrackPresentationSpec{
        .groupBy = TrackGroupKey::Artist, .sortBy = {TrackSortTerm{.field = TrackSortField::Artist}}});
      auto const resetEnd = std::chrono::steady_clock::now();

      auto const resetDuration = std::chrono::duration_cast<std::chrono::milliseconds>(resetEnd - resetStart);

      CHECK(receivedReset);
      CHECK(receivedSize == projSize);
      CHECK(resetDuration < std::chrono::seconds{2});
    }
  }
} // namespace ao::rt::test
