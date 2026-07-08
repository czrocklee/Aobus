// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/runtime/source/ManualListSourceTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/rt/source/ManualListSource.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>

namespace ao::rt::test
{
  // =============================================================================
  // Construction
  // =============================================================================
  TEST_CASE("ManualListSource - empty source starts without tracks or upstream source",
            "[runtime][unit][manual-list][construction]")
  {
    auto mls = ManualListSource{};

    CHECK(mls.size() == 0);
    CHECK(mls.trackIds().empty());
    CHECK(mls.source() == nullptr);
  }

  TEST_CASE("ManualListSource - construction from ListView copies tracks without upstream source",
            "[runtime][unit][manual-list][construction]")
  {
    auto lv = ListViewOwner{{TrackId{10}, TrackId{20}, TrackId{30}}};
    auto mls = ManualListSource{lv.view()};

    REQUIRE(mls.size() == 3);
    CHECK(mls.trackIdAt(0) == TrackId{10});
    CHECK(mls.trackIdAt(1) == TrackId{20});
    CHECK(mls.trackIdAt(2) == TrackId{30});
    CHECK(mls.source() == nullptr);
  }

  TEST_CASE("ManualListSource - construction from empty ListView creates empty list",
            "[runtime][unit][manual-list][construction]")
  {
    auto lv = ListViewOwner{{}};
    auto mls = ManualListSource{lv.view()};

    CHECK(mls.size() == 0);
    CHECK(mls.trackIds().empty());
  }

  TEST_CASE("ManualListSource - construction with upstream source attaches as observer",
            "[runtime][unit][manual-list][construction]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});
    source.addInitial(TrackId{2});

    auto lv = ListViewOwner{{TrackId{1}}};
    auto mls = ManualListSource{lv.view(), &source};

    CHECK(mls.source() == &source);
    CHECK(mls.size() == 1);
    CHECK(mls.trackIdAt(0) == TrackId{1});

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    source.update(TrackId{1});

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::Updated);
    CHECK(spy.events[0].id == TrackId{1});

    mls.detach(&spy);
  }

  // =============================================================================
  // TrackSource queries
  // =============================================================================
  TEST_CASE("ManualListSource - size returns number of manual tracks", "[runtime][unit][manual-list][query]")
  {
    auto mls = ManualListSource{};

    CHECK(mls.size() == 0);

    mls.trackIds().emplace_back(1);
    CHECK(mls.size() == 1);

    mls.trackIds().emplace_back(2);
    mls.trackIds().emplace_back(3);
    CHECK(mls.size() == 3);
  }

  TEST_CASE("ManualListSource - trackIdAt returns IDs in list order", "[runtime][unit][manual-list][query]")
  {
    auto lv = ListViewOwner{{TrackId{100}, TrackId{200}, TrackId{300}}};
    auto mls = ManualListSource{lv.view()};

    CHECK(mls.trackIdAt(0) == TrackId{100});
    CHECK(mls.trackIdAt(1) == TrackId{200});
    CHECK(mls.trackIdAt(2) == TrackId{300});
  }

  TEST_CASE("ManualListSource - indexOf returns local index for member track", "[runtime][unit][manual-list][query]")
  {
    auto lv = ListViewOwner{{TrackId{5}, TrackId{10}, TrackId{15}}};
    auto mls = ManualListSource{lv.view()};

    CHECK(mls.indexOf(TrackId{5}) == std::optional{std::size_t{0}});
    CHECK(mls.indexOf(TrackId{10}) == std::optional{std::size_t{1}});
    CHECK(mls.indexOf(TrackId{15}) == std::optional{std::size_t{2}});
  }

  TEST_CASE("ManualListSource - indexOf returns nullopt for non-members and empty lists",
            "[runtime][unit][manual-list][query]")
  {
    auto lv = ListViewOwner{{TrackId{1}, TrackId{2}}};
    auto mls = ManualListSource{lv.view()};

    CHECK(mls.indexOf(TrackId{99}) == std::nullopt);

    auto emptyMls = ManualListSource{};
    CHECK(emptyMls.indexOf(TrackId{1}) == std::nullopt);
  }

  TEST_CASE("ManualListSource - contains distinguishes members from non-members", "[runtime][unit][manual-list][query]")
  {
    auto lv = ListViewOwner{{TrackId{42}, TrackId{43}}};
    auto mls = ManualListSource{lv.view()};

    CHECK(mls.contains(TrackId{42}));
    CHECK(mls.contains(TrackId{43}));
    CHECK_FALSE(mls.contains(TrackId{1}));
    CHECK_FALSE(mls.contains(TrackId{99}));
  }

  // =============================================================================
  // ListView reload
  // =============================================================================
  TEST_CASE("ManualListSource - reload replaces tracks when no upstream source exists",
            "[runtime][unit][manual-list][reload]")
  {
    auto lv1 = ListViewOwner{{TrackId{1}, TrackId{2}}};
    auto mls = ManualListSource{lv1.view()};

    auto lv2 = ListViewOwner{{TrackId{10}, TrackId{20}, TrackId{30}}};
    mls.reloadFromListView(lv2.view());

    REQUIRE(mls.size() == 3);
    CHECK(mls.trackIdAt(0) == TrackId{10});
    CHECK(mls.trackIdAt(1) == TrackId{20});
    CHECK(mls.trackIdAt(2) == TrackId{30});
  }

  TEST_CASE("ManualListSource - reload filters tracks against upstream source", "[runtime][unit][manual-list][reload]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});
    source.addInitial(TrackId{3});
    source.addInitial(TrackId{5});

    auto lv = ListViewOwner{{TrackId{1}, TrackId{3}, TrackId{5}}};
    auto mls = ManualListSource{lv.view(), &source};

    auto lv2 = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}, TrackId{4}}};
    mls.reloadFromListView(lv2.view());

    REQUIRE(mls.size() == 2);
    CHECK(mls.trackIdAt(0) == TrackId{1});
    CHECK(mls.trackIdAt(1) == TrackId{3});
  }

  TEST_CASE("ManualListSource - reload notifies observers with reset", "[runtime][unit][manual-list][reload]")
  {
    auto lv1 = ListViewOwner{{TrackId{7}, TrackId{8}}};
    auto mls = ManualListSource{lv1.view()};

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    auto lv2 = ListViewOwner{{TrackId{9}}};
    mls.reloadFromListView(lv2.view());

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::Reset);
    REQUIRE(mls.size() == 1);
    CHECK(mls.trackIdAt(0) == TrackId{9});

    mls.detach(&spy);
  }

  TEST_CASE("ManualListSource - reload from empty view clears list and notifies reset",
            "[runtime][unit][manual-list][reload]")
  {
    auto lv = ListViewOwner{{TrackId{1}, TrackId{2}}};
    auto mls = ManualListSource{lv.view()};

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    auto emptyLv = ListViewOwner{{}};
    mls.reloadFromListView(emptyLv.view());

    CHECK(mls.size() == 0);
    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::Reset);

    mls.detach(&spy);
  }

  TEST_CASE("ManualListSource - reload re-filters after upstream removals", "[runtime][unit][manual-list][reload]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});
    source.addInitial(TrackId{2});

    auto lv = ListViewOwner{{TrackId{1}, TrackId{2}}};
    auto mls = ManualListSource{lv.view(), &source};

    source.remove(TrackId{1});

    auto lv2 = ListViewOwner{{TrackId{1}, TrackId{2}}};
    mls.reloadFromListView(lv2.view());

    REQUIRE(mls.size() == 1);
    CHECK(mls.trackIdAt(0) == TrackId{2});
  }

  TEST_CASE("ManualListSource - reload filters all tracks absent from upstream source",
            "[runtime][unit][manual-list][reload]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});
    source.addInitial(TrackId{2});

    auto lv = ListViewOwner{{TrackId{1}, TrackId{2}}};
    auto mls = ManualListSource{lv.view(), &source};

    auto lv2 = ListViewOwner{{TrackId{10}, TrackId{20}, TrackId{30}}};
    mls.reloadFromListView(lv2.view());

    CHECK(mls.size() == 0);
  }
} // namespace ao::rt::test
