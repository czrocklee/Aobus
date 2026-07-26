// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/source/TrackSourceDeltaBuilder.h"

#include <ao/CoreIds.h>
#include <ao/rt/TrackEditScript.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    std::vector<TrackId> replay(std::vector<TrackId> ids, delta::RegularTrackEditScript const& script)
    {
      REQUIRE(delta::validate(script, ids.size()));

      for (auto const& edit : script.edits)
      {
        if (auto const* removal = std::get_if<delta::RemoveRange>(&edit); removal != nullptr)
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

        auto const* insertion = std::get_if<delta::InsertRange>(&edit);
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

    auto const optScript = builder.build();

    REQUIRE(optScript);
    REQUIRE(optScript->edits.size() == 2);
    auto const& first = std::get<delta::RemoveRange>(optScript->edits[0]);
    CHECK(first.start == 4);
    CHECK(first.trackIds == std::vector{TrackId{50}});
    auto const& second = std::get<delta::RemoveRange>(optScript->edits[1]);
    CHECK(second.start == 1);
    CHECK(second.trackIds == std::vector{TrackId{20}, TrackId{30}});
    CHECK(replay({TrackId{10}, TrackId{20}, TrackId{30}, TrackId{40}, TrackId{50}, TrackId{60}}, *optScript) ==
          std::vector{TrackId{10}, TrackId{40}, TrackId{60}});
  }

  TEST_CASE("TrackSourceDeltaBuilder - non-contiguous move inserts at the post-removal gap", "[runtime][unit][source]")
  {
    auto builder = TrackSourceDeltaBuilder{6};
    builder.remove(4, TrackId{50});
    builder.remove(1, TrackId{20});
    builder.insert(3, TrackId{50});
    builder.insert(2, TrackId{20});

    auto const optScript = builder.build();

    REQUIRE(optScript);
    REQUIRE(optScript->edits.size() == 3);
    CHECK(std::get<delta::RemoveRange>(optScript->edits[0]).start == 4);
    CHECK(std::get<delta::RemoveRange>(optScript->edits[1]).start == 1);
    auto const& insertion = std::get<delta::InsertRange>(optScript->edits[2]);
    CHECK(insertion.start == 2);
    CHECK(insertion.trackIds == std::vector{TrackId{20}, TrackId{50}});
    CHECK(replay({TrackId{10}, TrackId{20}, TrackId{30}, TrackId{40}, TrackId{50}, TrackId{60}}, *optScript) ==
          std::vector{TrackId{10}, TrackId{30}, TrackId{20}, TrackId{50}, TrackId{40}, TrackId{60}});
  }

  TEST_CASE("TrackSourceDeltaBuilder - ascending insertions use sequential resulting coordinates",
            "[runtime][unit][source]")
  {
    auto builder = TrackSourceDeltaBuilder{3};
    builder.insert(4, TrackId{60});
    builder.insert(2, TrackId{50});
    builder.insert(1, TrackId{40});

    auto const optScript = builder.build();

    REQUIRE(optScript);
    REQUIRE(optScript->edits.size() == 2);
    auto const& first = std::get<delta::InsertRange>(optScript->edits[0]);
    CHECK(first.start == 1);
    CHECK(first.trackIds == std::vector{TrackId{40}, TrackId{50}});
    auto const& second = std::get<delta::InsertRange>(optScript->edits[1]);
    CHECK(second.start == 4);
    CHECK(second.trackIds == std::vector{TrackId{60}});
    CHECK(replay({TrackId{10}, TrackId{20}, TrackId{30}}, *optScript) ==
          std::vector{TrackId{10}, TrackId{40}, TrackId{50}, TrackId{20}, TrackId{60}, TrackId{30}});
  }

  TEST_CASE("TrackSourceDeltaBuilder - no registered identities produce no batch", "[runtime][unit][source]")
  {
    auto const builder = TrackSourceDeltaBuilder{4};

    CHECK_FALSE(builder.build());
  }
} // namespace ao::rt::test
