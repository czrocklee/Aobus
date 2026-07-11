// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/source/TrackSourceDeltaBuilder.h"

#include <ao/CoreIds.h>
#include <ao/rt/source/TrackSourceDelta.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    std::vector<TrackId> replay(std::vector<TrackId> ids, TrackSourceDeltaBatch const& batch)
    {
      REQUIRE(validateTrackSourceDeltaBatch(batch, ids.size()));

      for (auto const& delta : batch.deltas)
      {
        if (auto const* removal = std::get_if<SourceRemoveRange>(&delta); removal != nullptr)
        {
          REQUIRE(removal->start + removal->trackIds.size() <= ids.size());
          auto const actual =
            std::vector<TrackId>(ids.begin() + static_cast<std::ptrdiff_t>(removal->start),
                                 ids.begin() + static_cast<std::ptrdiff_t>(removal->start + removal->trackIds.size()));
          REQUIRE(actual == removal->trackIds);
          ids.erase(ids.begin() + static_cast<std::ptrdiff_t>(removal->start),
                    ids.begin() + static_cast<std::ptrdiff_t>(removal->start + removal->trackIds.size()));
          continue;
        }

        auto const* insertion = std::get_if<SourceInsertRange>(&delta);
        REQUIRE(insertion != nullptr);
        REQUIRE(insertion->start <= ids.size());
        ids.insert(ids.begin() + static_cast<std::ptrdiff_t>(insertion->start),
                   insertion->trackIds.begin(),
                   insertion->trackIds.end());
      }

      return ids;
    }
  } // namespace

  TEST_CASE("TrackSourceDeltaBuilder - non-contiguous removals are coalesced in descending order",
            "[runtime][unit][source]")
  {
    auto builder = TrackSourceDeltaBuilder{6};
    builder.remove(2, TrackId{30});
    builder.remove(4, TrackId{50});
    builder.remove(1, TrackId{20});

    auto const optBatch = builder.build();

    REQUIRE(optBatch);
    REQUIRE(optBatch->deltas.size() == 2);
    auto const& first = std::get<SourceRemoveRange>(optBatch->deltas[0]);
    CHECK(first.start == 4);
    CHECK(first.trackIds == std::vector{TrackId{50}});
    auto const& second = std::get<SourceRemoveRange>(optBatch->deltas[1]);
    CHECK(second.start == 1);
    CHECK(second.trackIds == std::vector{TrackId{20}, TrackId{30}});
    CHECK(replay({TrackId{10}, TrackId{20}, TrackId{30}, TrackId{40}, TrackId{50}, TrackId{60}}, *optBatch) ==
          std::vector{TrackId{10}, TrackId{40}, TrackId{60}});
  }

  TEST_CASE("TrackSourceDeltaBuilder - non-contiguous move inserts at the post-removal gap", "[runtime][unit][source]")
  {
    auto builder = TrackSourceDeltaBuilder{6};
    builder.remove(4, TrackId{50});
    builder.remove(1, TrackId{20});
    builder.insert(3, TrackId{50});
    builder.insert(2, TrackId{20});

    auto const optBatch = builder.build();

    REQUIRE(optBatch);
    REQUIRE(optBatch->deltas.size() == 3);
    CHECK(std::get<SourceRemoveRange>(optBatch->deltas[0]).start == 4);
    CHECK(std::get<SourceRemoveRange>(optBatch->deltas[1]).start == 1);
    auto const& insertion = std::get<SourceInsertRange>(optBatch->deltas[2]);
    CHECK(insertion.start == 2);
    CHECK(insertion.trackIds == std::vector{TrackId{20}, TrackId{50}});
    CHECK(replay({TrackId{10}, TrackId{20}, TrackId{30}, TrackId{40}, TrackId{50}, TrackId{60}}, *optBatch) ==
          std::vector{TrackId{10}, TrackId{30}, TrackId{20}, TrackId{50}, TrackId{40}, TrackId{60}});
  }

  TEST_CASE("TrackSourceDeltaBuilder - ascending insertions use sequential resulting coordinates",
            "[runtime][unit][source]")
  {
    auto builder = TrackSourceDeltaBuilder{3};
    builder.insert(4, TrackId{60});
    builder.insert(2, TrackId{50});
    builder.insert(1, TrackId{40});

    auto const optBatch = builder.build();

    REQUIRE(optBatch);
    REQUIRE(optBatch->deltas.size() == 2);
    auto const& first = std::get<SourceInsertRange>(optBatch->deltas[0]);
    CHECK(first.start == 1);
    CHECK(first.trackIds == std::vector{TrackId{40}, TrackId{50}});
    auto const& second = std::get<SourceInsertRange>(optBatch->deltas[1]);
    CHECK(second.start == 4);
    CHECK(second.trackIds == std::vector{TrackId{60}});
    CHECK(replay({TrackId{10}, TrackId{20}, TrackId{30}}, *optBatch) ==
          std::vector{TrackId{10}, TrackId{40}, TrackId{50}, TrackId{20}, TrackId{60}, TrackId{30}});
  }

  TEST_CASE("TrackSourceDeltaBuilder - no registered identities produce no batch", "[runtime][unit][source]")
  {
    auto const builder = TrackSourceDeltaBuilder{4};

    CHECK_FALSE(builder.build());
  }
} // namespace ao::rt::test
