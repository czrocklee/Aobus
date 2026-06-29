// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/rt/NavigationHistory.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::rt::test
{
  namespace
  {
    auto makeSpec(std::string_view id, TrackGroupKey groupBy = TrackGroupKey::None)
    {
      return TrackPresentationSpec{.id = std::string{id}, .groupBy = groupBy};
    }

    auto makePoint(ListId listId, std::string filterExpression = {}, TrackPresentationSpec presentation = {})
    {
      if (presentation.id.empty())
      {
        presentation = makeSpec("songs");
      }

      return NavigationPoint{
        .listId = listId, .filterExpression = std::move(filterExpression), .presentation = std::move(presentation)};
    }
  } // namespace

  TEST_CASE("NavigationHistory - empty history reports no current point or movement",
            "[runtime][unit][navigation][lifecycle]")
  {
    auto h = NavigationHistory{};
    CHECK(h.size() == 0);
    CHECK_FALSE(h.currentIndex().has_value());
    CHECK_FALSE(h.canGoBack());
    CHECK_FALSE(h.canGoForward());
    CHECK_FALSE(h.current().has_value());
    CHECK_FALSE(h.back().has_value());
    CHECK_FALSE(h.forward().has_value());
  }

  TEST_CASE("NavigationHistory - zero max size is accepted as an empty history",
            "[runtime][unit][navigation][lifecycle][capacity]")
  {
    auto const h = NavigationHistory{0};
    // Should not crash; max size is clamped to at least 1 internally.
    CHECK(h.size() == 0);
  }

  TEST_CASE("NavigationHistory - default construction starts without entries", "[runtime][unit][navigation][lifecycle]")
  {
    auto const h = NavigationHistory{};
    // Default is 256; only observable via eviction tests below.
    CHECK(h.size() == 0);
  }

  TEST_CASE("NavigationHistory - first commit becomes the current point", "[runtime][unit][navigation][commit]")
  {
    auto h = NavigationHistory{};
    auto const point = makePoint(ListId{10});
    h.commit(point);

    CHECK(h.size() == 1);
    CHECK(h.currentIndex() == 0);
    CHECK(h.current() == point);
    CHECK_FALSE(h.canGoBack());
    CHECK_FALSE(h.canGoForward());
  }

  TEST_CASE("NavigationHistory - committing a second point enables backward navigation",
            "[runtime][unit][navigation][commit][traversal]")
  {
    auto h = NavigationHistory{};
    h.commit(makePoint(ListId{10}));
    h.commit(makePoint(ListId{20}));

    CHECK(h.size() == 2);
    CHECK(h.currentIndex() == 1);
    CHECK(h.current()->listId == ListId{20});
    CHECK(h.canGoBack());
    CHECK_FALSE(h.canGoForward());
  }

  TEST_CASE("NavigationHistory - committing multiple points keeps the newest point current",
            "[runtime][unit][navigation][commit]")
  {
    auto h = NavigationHistory{};
    h.commit(makePoint(ListId{10}));
    h.commit(makePoint(ListId{20}));
    h.commit(makePoint(ListId{30}));

    CHECK(h.size() == 3);
    CHECK(h.currentIndex() == 2);
    CHECK(h.current()->listId == ListId{30});
    CHECK(h.canGoBack());
    CHECK_FALSE(h.canGoForward());
  }

  TEST_CASE("NavigationHistory - committing the current point is deduplicated",
            "[runtime][unit][navigation][commit][dedup]")
  {
    auto h = NavigationHistory{};
    auto const point = makePoint(ListId{10});
    h.commit(point);
    h.commit(point);

    CHECK(h.size() == 1);
    CHECK(h.currentIndex() == 0);
  }

  TEST_CASE("NavigationHistory - presentation changes create distinct points",
            "[runtime][unit][navigation][commit][dedup]")
  {
    auto h = NavigationHistory{};
    auto const pointA = makePoint(ListId{10}, {}, makeSpec("songs"));
    auto const pointB = makePoint(ListId{10}, {}, makeSpec("albums", TrackGroupKey::Album));
    h.commit(pointA);
    h.commit(pointB);

    CHECK(h.size() == 2);
  }

  TEST_CASE("NavigationHistory - filter changes create distinct points", "[runtime][unit][navigation][commit][dedup]")
  {
    auto h = NavigationHistory{};
    h.commit(makePoint(ListId{10}));
    h.commit(makePoint(ListId{10}, "genre == \"Rock\""));

    CHECK(h.size() == 2);
  }

  TEST_CASE("NavigationHistory - list changes create distinct points", "[runtime][unit][navigation][commit][dedup]")
  {
    auto h = NavigationHistory{};
    h.commit(makePoint(ListId{10}));
    h.commit(makePoint(ListId{20}));

    CHECK(h.size() == 2);
  }

  TEST_CASE("NavigationHistory - back moves from the newest point to the previous point",
            "[runtime][unit][navigation][traversal]")
  {
    auto h = NavigationHistory{};
    auto const a = makePoint(ListId{10});
    auto const b = makePoint(ListId{20});
    h.commit(a);
    h.commit(b);

    auto const optResult = h.back();
    REQUIRE(optResult.has_value());
    CHECK(*optResult == a);
    CHECK(h.currentIndex() == 0);
    CHECK_FALSE(h.canGoBack());
    CHECK(h.canGoForward());
  }

  TEST_CASE("NavigationHistory - back from the middle keeps both directions available",
            "[runtime][unit][navigation][traversal]")
  {
    auto h = NavigationHistory{};
    h.commit(makePoint(ListId{10}));
    h.commit(makePoint(ListId{20}));
    h.commit(makePoint(ListId{30}));

    auto const optResult = h.back();
    REQUIRE(optResult.has_value());
    CHECK(optResult->listId == ListId{20});
    CHECK(h.currentIndex() == 1);
    CHECK(h.canGoBack());
    CHECK(h.canGoForward());
  }

  TEST_CASE("NavigationHistory - back at the first point leaves the current index unchanged",
            "[runtime][unit][navigation][traversal][boundary]")
  {
    auto h = NavigationHistory{};
    h.commit(makePoint(ListId{10}));

    auto const optResult = h.back();
    CHECK_FALSE(optResult.has_value());
    CHECK(h.currentIndex() == 0);
  }

  TEST_CASE("NavigationHistory - back on empty history returns no point",
            "[runtime][unit][navigation][traversal][boundary]")
  {
    auto h = NavigationHistory{};
    CHECK_FALSE(h.back().has_value());
  }

  TEST_CASE("NavigationHistory - forward after back restores the newer point", "[runtime][unit][navigation][traversal]")
  {
    auto h = NavigationHistory{};
    h.commit(makePoint(ListId{10}));
    h.commit(makePoint(ListId{20}));
    h.commit(makePoint(ListId{30}));
    h.back();

    auto const optResult = h.forward();
    REQUIRE(optResult.has_value());
    CHECK(optResult->listId == ListId{30});
    CHECK(h.currentIndex() == 2);
    CHECK_FALSE(h.canGoForward());
  }

  TEST_CASE("NavigationHistory - forward at the newest point leaves the current index unchanged",
            "[runtime][unit][navigation][traversal][boundary]")
  {
    auto h = NavigationHistory{};
    h.commit(makePoint(ListId{10}));

    CHECK_FALSE(h.forward().has_value());
    CHECK(h.currentIndex() == 0);
  }

  TEST_CASE("NavigationHistory - forward on empty history returns no point",
            "[runtime][unit][navigation][traversal][boundary]")
  {
    auto h = NavigationHistory{};
    CHECK_FALSE(h.forward().has_value());
  }

  TEST_CASE("NavigationHistory - forward can advance from an earlier committed point",
            "[runtime][unit][navigation][traversal]")
  {
    auto h = NavigationHistory{};
    h.commit(makePoint(ListId{10})); // A
    h.commit(makePoint(ListId{20})); // B
    h.commit(makePoint(ListId{30})); // C
    h.commit(makePoint(ListId{40})); // D

    h.back();                        // to C
    h.back();                        // to B
    auto const optFwd = h.forward(); // to C
    REQUIRE(optFwd.has_value());
    CHECK(optFwd->listId == ListId{30});
  }

  TEST_CASE("NavigationHistory - backward and forward traversal can round-trip to the newest point",
            "[runtime][unit][navigation][traversal]")
  {
    auto h = NavigationHistory{};
    h.commit(makePoint(ListId{10})); // A
    h.commit(makePoint(ListId{20})); // B
    h.commit(makePoint(ListId{30})); // C

    h.back();                        // to B
    h.back();                        // to A
    h.forward();                     // to B
    auto const optFwd = h.forward(); // to C
    REQUIRE(optFwd.has_value());
    CHECK(optFwd->listId == ListId{30});
  }

  TEST_CASE("NavigationHistory - committing after back truncates forward history",
            "[runtime][unit][navigation][commit][traversal]")
  {
    auto h = NavigationHistory{};
    h.commit(makePoint(ListId{10})); // A
    h.commit(makePoint(ListId{20})); // B
    h.commit(makePoint(ListId{30})); // C

    h.back();                        // to B
    h.commit(makePoint(ListId{40})); // D

    CHECK(h.size() == 3);
    CHECK(h.currentIndex() == 2);
    CHECK(h.current()->listId == ListId{40});
    // C is gone
    CHECK_FALSE(h.canGoForward());
  }

  TEST_CASE("NavigationHistory - committing from the first point removes all future entries",
            "[runtime][unit][navigation][commit][traversal]")
  {
    auto h = NavigationHistory{};
    h.commit(makePoint(ListId{10})); // A
    h.commit(makePoint(ListId{20})); // B
    h.commit(makePoint(ListId{30})); // C

    h.back();                        // to B
    h.back();                        // to A
    h.commit(makePoint(ListId{40})); // D

    CHECK(h.size() == 2); // A, D (B and C truncated)
    CHECK(h.currentIndex() == 1);
    CHECK(h.current()->listId == ListId{40});
  }

  TEST_CASE("NavigationHistory - capacity eviction removes the oldest point",
            "[runtime][unit][navigation][capacity][eviction]")
  {
    auto h = NavigationHistory{3};
    h.commit(makePoint(ListId{10})); // A
    h.commit(makePoint(ListId{20})); // B
    h.commit(makePoint(ListId{30})); // C
    h.commit(makePoint(ListId{40})); // D

    CHECK(h.size() == 3);
    CHECK(h.currentIndex() == 2);
    CHECK(h.current()->listId == ListId{40});

    auto const optFirstBack = h.back();
    REQUIRE(optFirstBack.has_value());
    CHECK(optFirstBack->listId == ListId{30});
  }

  TEST_CASE("NavigationHistory - capacity one keeps only the newest point",
            "[runtime][unit][navigation][capacity][eviction]")
  {
    auto h = NavigationHistory{1};
    h.commit(makePoint(ListId{10}));
    h.commit(makePoint(ListId{20}));

    CHECK(h.size() == 1);
    CHECK(h.currentIndex() == 0);
    CHECK(h.current()->listId == ListId{20});
    CHECK_FALSE(h.canGoBack());
  }

  TEST_CASE("NavigationHistory - capacity one never enables backward navigation",
            "[runtime][unit][navigation][capacity][traversal]")
  {
    auto h = NavigationHistory{1};
    h.commit(makePoint(ListId{10}));

    CHECK_FALSE(h.back().has_value());
  }

  TEST_CASE("NavigationHistory - current on empty history returns no point", "[runtime][unit][navigation][current]")
  {
    auto const h = NavigationHistory{};
    CHECK_FALSE(h.current().has_value());
  }

  TEST_CASE("NavigationHistory - current returns the last committed point", "[runtime][unit][navigation][current]")
  {
    auto h = NavigationHistory{};
    auto const point = makePoint(ListId{10});
    h.commit(point);
    CHECK(h.current() == point);
  }

  TEST_CASE("NavigationHistory - current follows backward traversal", "[runtime][unit][navigation][current]")
  {
    auto h = NavigationHistory{};
    auto const a = makePoint(ListId{10});
    h.commit(a);
    h.commit(makePoint(ListId{20}));
    h.back();
    CHECK(h.current() == a);
  }

  TEST_CASE("NavigationHistory - current returns a copy of the stored point", "[runtime][unit][navigation][current]")
  {
    auto h = NavigationHistory{};
    auto const point = makePoint(ListId{10});
    h.commit(point);

    auto copy = *h.current();
    copy.listId = ListId{99};

    CHECK(h.current()->listId == ListId{10});
  }

  TEST_CASE("NavigationHistory - committing at the first point truncates forward history",
            "[runtime][unit][navigation][commit][traversal][boundary]")
  {
    auto h = NavigationHistory{};
    h.commit(makePoint(ListId{10})); // A
    h.commit(makePoint(ListId{20})); // B

    h.back(); // to A
    CHECK_FALSE(h.canGoBack());
    CHECK(h.canGoForward());

    h.commit(makePoint(ListId{30})); // new commit truncates B
    CHECK(h.size() == 2);            // A, C
    CHECK_FALSE(h.canGoForward());
  }
} // namespace ao::rt::test
