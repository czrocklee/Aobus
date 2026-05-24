// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TestUtils.h"
#include "ao/Type.h"
#include <ao/rt/ProjectionTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackListProjection.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/TrackSource.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    class LargeTrackSource final : public TrackSource
    {
    public:
      void setIds(std::vector<TrackId> ids) { _ids = std::move(ids); }
      void batchInsert(std::span<TrackId const> ids)
      {
        _ids.insert(_ids.end(), ids.begin(), ids.end());
        notifyInserted(ids);
      }

      std::size_t size() const override { return _ids.size(); }
      TrackId trackIdAt(std::size_t index) const override { return _ids.at(index); }
      std::optional<std::size_t> indexOf(TrackId id) const override
      {
        for (std::size_t idx = 0; idx < _ids.size(); ++idx)
        {
          if (_ids[idx] == id)
          {
            return idx;
          }
        }

        return std::nullopt;
      }

    private:
      std::vector<TrackId> _ids;
    };
  }

  TEST_CASE("TrackListProjection - 10k Scale Performance", "[app][runtime][projection][scale]")
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

    auto source = LargeTrackSource{};
    source.setIds(ids);

    auto const start = std::chrono::steady_clock::now();
    auto proj = TrackListProjection{ViewId{1}, source, lib};
    proj.setPresentation(
      TrackPresentationSpec{.groupBy = TrackGroupKey::None, .sortBy = {TrackSortTerm{.field = TrackSortField::Title}}});
    auto const end = std::chrono::steady_clock::now();

    auto const duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    CHECK(proj.size() == kTrackCount);
    // Initial 10k sort + normalization should be reasonably fast with our cache.
    // We expect it to be well under 500ms on a typical dev machine.
    CHECK(duration.count() < 1000);

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
      CHECK(batchDuration.count() < 100);
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
      CHECK(firstDuration.count() < 5000);
      CHECK(midDuration.count() < 5000);
      CHECK(lastDuration.count() < 5000);

      // O(1) property: mid and last should be within 2x of first
      auto const baseline = std::max(std::int64_t{1}, firstDuration.count());
      auto const ratioMid = static_cast<double>(midDuration.count()) / static_cast<double>(baseline);
      auto const ratioLast = static_cast<double>(lastDuration.count()) / static_cast<double>(baseline);
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
      CHECK(resetDuration.count() < 2000);
    }
  }
}
