// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/track/IndexedTrackRowCache.h>

#include <ao/CoreIds.h>
#include <ao/rt/TrackRow.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ao::uimodel::test
{
  TEST_CASE("IndexedTrackRowCache - lazily loads rows and retains the bounded working set",
            "[uimodel][unit][track-cache]")
  {
    std::size_t loadCount = 0;
    auto cache = IndexedTrackRowCache{2};
    cache.reset(4,
                [&loadCount](std::size_t const index) -> std::optional<rt::TrackRow>
                {
                  ++loadCount;
                  return rt::TrackRow{.id = TrackId{static_cast<std::uint32_t>(index + 1)}, .title = "row"};
                });

    REQUIRE(cache.rowAt(0) != nullptr);
    CHECK(cache.rowAt(0)->id == TrackId{1});
    CHECK(loadCount == 1);

    REQUIRE(cache.rowAt(1) != nullptr);
    REQUIRE(cache.rowAt(0) != nullptr);
    REQUIRE(cache.rowAt(2) != nullptr);

    CHECK(cache.contains(0));
    CHECK_FALSE(cache.contains(1));
    CHECK(cache.contains(2));
    CHECK(loadCount == 3);
  }

  TEST_CASE("IndexedTrackRowCache - source replacement invalidates rows and bounds checks before loading",
            "[uimodel][unit][track-cache]")
  {
    std::size_t loadCount = 0;
    auto cache = IndexedTrackRowCache{};
    cache.reset(1,
                [&loadCount](std::size_t) -> std::optional<rt::TrackRow>
                {
                  ++loadCount;
                  return rt::TrackRow{.id = TrackId{7}, .title = "before"};
                });
    REQUIRE(cache.rowAt(0) != nullptr);

    cache.reset(1,
                [&loadCount](std::size_t) -> std::optional<rt::TrackRow>
                {
                  ++loadCount;
                  return rt::TrackRow{.id = TrackId{8}, .title = "after"};
                });

    CHECK_FALSE(cache.contains(0));
    CHECK(cache.rowAt(1) == nullptr);
    CHECK(loadCount == 1);
    REQUIRE(cache.rowAt(0) != nullptr);
    CHECK(cache.rowAt(0)->id == TrackId{8});
    CHECK(loadCount == 2);
  }

  TEST_CASE("IndexedTrackRowCache - retries missing rows without consuming cache capacity",
            "[uimodel][unit][track-cache]")
  {
    auto loads = std::array<std::size_t, 3>{};
    auto cache = IndexedTrackRowCache{1};
    cache.reset(3,
                [&loads](std::size_t const index) -> std::optional<rt::TrackRow>
                {
                  ++loads[index];

                  if ((index == 1 && loads[index] == 1) || index == 2)
                  {
                    return std::nullopt;
                  }

                  return rt::TrackRow{.id = TrackId{static_cast<std::uint32_t>(index + 1)}, .title = "retained"};
                });

    REQUIRE(cache.rowAt(0) != nullptr);
    CHECK(cache.rowAt(0)->id == TrackId{1});
    REQUIRE(cache.contains(0));
    CHECK(cache.rowAt(1) == nullptr);
    CHECK_FALSE(cache.contains(1));
    CHECK(cache.contains(0));
    REQUIRE(cache.rowAt(0) != nullptr);
    CHECK(loads == std::array<std::size_t, 3>{1, 1, 0});

    REQUIRE(cache.rowAt(1) != nullptr);
    CHECK(cache.rowAt(1)->id == TrackId{2});
    CHECK_FALSE(cache.contains(0));
    CHECK(cache.contains(1));
    CHECK(loads == std::array<std::size_t, 3>{1, 2, 0});

    CHECK(cache.rowAt(2) == nullptr);
    CHECK(cache.rowAt(2) == nullptr);
    CHECK_FALSE(cache.contains(2));
    CHECK(cache.contains(1));
    REQUIRE(cache.rowAt(1) != nullptr);
    CHECK(loads == std::array<std::size_t, 3>{1, 2, 2});
  }
} // namespace ao::uimodel::test
