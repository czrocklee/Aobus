// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include <ao/library/MusicLibrary.h>
#include <runtime/TrackListProjection.h>
#include <runtime/TrackSource.h>

#include "TestUtils.h"

#include <chrono>
#include <format>
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
    std::vector<TrackId> ids;
    ids.reserve(kTrackCount);

    for (int idx = 0; idx < kTrackCount; ++idx)
    {
      ids.push_back(env.addTrack(TrackSpec{.title = std::format("Track {:05d}", idx),
                                           .artist = std::format("Artist {:03d}", idx % 100),
                                           .album = std::format("Album {:03d}", idx % 500)}));
    }

    LargeTrackSource source;
    source.setIds(ids);

    auto start = std::chrono::steady_clock::now();
    TrackListProjection proj{ViewId{1}, source, lib};
    proj.setPresentation(TrackGroupKey::None, {TrackSortTerm{.field = TrackSortField::Title}});
    auto end = std::chrono::steady_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    CHECK(proj.size() == kTrackCount);
    // Initial 10k sort + normalization should be reasonably fast with our cache.
    // We expect it to be well under 500ms on a typical dev machine.
    CHECK(duration.count() < 1000);

    SECTION("Batch insertion performance (incremental merge)")
    {
      std::vector<TrackId> newIds;
      newIds.reserve(100);

      for (int idx = 0; idx < 100; ++idx)
      {
        newIds.push_back(env.addTrack(TrackSpec{.title = std::format("New Track {:05d}", idx)}));
      }

      auto batchStart = std::chrono::steady_clock::now();
      // Trigger batch insertion via source
      source.batchInsert(newIds);
      auto batchEnd = std::chrono::steady_clock::now();

      auto batchDuration = std::chrono::duration_cast<std::chrono::milliseconds>(batchEnd - batchStart);

      CHECK(proj.size() == kTrackCount + 100);
      // Merging 100 into 10k should be very fast (< 50ms)
      CHECK(batchDuration.count() < 100);
    }
  }
}
