// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/runtime/source/ManualListSourceTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/rt/source/ManualListSource.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <span>

namespace ao::rt::test
{
  // =============================================================================
  // Reset forwarding
  // =============================================================================
  TEST_CASE("ManualListSource - upstream reset is ignored without upstream source",
            "[runtime][unit][manual-list][reset]")
  {
    auto lv = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
    auto mls = ManualListSource{lv.view()};

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    mls.onReset();

    CHECK(spy.events.empty());
    CHECK(mls.size() == 3);
    CHECK(mls.trackIds().size() == 3);

    mls.detach(&spy);
  }

  TEST_CASE("ManualListSource - upstream reset filters stale IDs and notifies observers",
            "[runtime][unit][manual-list][reset]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});
    source.addInitial(TrackId{2});
    source.addInitial(TrackId{3});

    auto lv = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
    auto mls = ManualListSource{lv.view(), &source};

    mls.trackIds().emplace_back(99);

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    source.reset({{TrackId{1}, TrackId{3}}});

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::Reset);
    REQUIRE(mls.size() == 2);
    CHECK(mls.trackIdAt(0) == TrackId{1});
    CHECK(mls.trackIdAt(1) == TrackId{3});

    mls.detach(&spy);
  }

  TEST_CASE("ManualListSource - upstream reset clears all tracks when source becomes empty",
            "[runtime][unit][manual-list][reset]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});
    source.addInitial(TrackId{2});

    auto lv = ListViewOwner{{TrackId{1}, TrackId{2}}};
    auto mls = ManualListSource{lv.view(), &source};

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    source.reset();

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::Reset);
    CHECK(mls.size() == 0);
    CHECK(mls.trackIds().empty());

    mls.detach(&spy);
  }

  TEST_CASE("ManualListSource - upstream reset keeps retained tracks still present in source",
            "[runtime][unit][manual-list][reset]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});
    source.addInitial(TrackId{2});
    source.addInitial(TrackId{3});

    auto lv = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
    auto mls = ManualListSource{lv.view(), &source};

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    source.reset({{TrackId{1}, TrackId{2}, TrackId{3}}});

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::Reset);
    REQUIRE(mls.size() == 3);
    CHECK(mls.trackIdAt(0) == TrackId{1});
    CHECK(mls.trackIdAt(1) == TrackId{2});
    CHECK(mls.trackIdAt(2) == TrackId{3});

    mls.detach(&spy);
  }

  // =============================================================================
  TEST_CASE("ManualListSource - upstream insertion does not mutate manual membership",
            "[runtime][unit][manual-list][insert]")
  {
    auto lv = ListViewOwner{{TrackId{1}, TrackId{2}}};
    auto mls = ManualListSource{lv.view()};

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    mls.onInserted(TrackId{99}, 0);

    CHECK(spy.events.empty());
    CHECK(mls.size() == 2);
    CHECK_FALSE(mls.contains(TrackId{99}));

    mls.detach(&spy);
  }

  TEST_CASE("ManualListSource - upstream batch insertion does not mutate manual membership",
            "[runtime][unit][manual-list][insert]")
  {
    auto lv = ListViewOwner{{TrackId{1}}};
    auto mls = ManualListSource{lv.view()};

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    auto const batch = std::array{TrackId{10}, TrackId{20}};
    mls.onBulkInserted(std::span{batch});

    CHECK(spy.events.empty());
    CHECK(mls.size() == 1);

    mls.detach(&spy);
  }

  // =============================================================================
  // Update forwarding
  // =============================================================================
  TEST_CASE("ManualListSource - upstream update forwards member tracks with local index",
            "[runtime][unit][manual-list][update]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});
    source.addInitial(TrackId{2});
    source.addInitial(TrackId{3});

    auto lv = ListViewOwner{{TrackId{1}, TrackId{3}}};
    auto mls = ManualListSource{lv.view(), &source};

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    source.update(TrackId{3});

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::Updated);
    CHECK(spy.events[0].id == TrackId{3});
    CHECK(spy.events[0].index == 1);

    spy.clear();
    source.update(TrackId{1});

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::Updated);
    CHECK(spy.events[0].id == TrackId{1});
    CHECK(spy.events[0].index == 0);

    mls.detach(&spy);
  }

  TEST_CASE("ManualListSource - upstream update ignores non-member tracks", "[runtime][unit][manual-list][update]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});
    source.addInitial(TrackId{99});

    auto lv = ListViewOwner{{TrackId{1}}};
    auto mls = ManualListSource{lv.view(), &source};

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    source.update(TrackId{99});

    CHECK(spy.events.empty());
    CHECK(mls.size() == 1);

    mls.detach(&spy);
  }

  TEST_CASE("ManualListSource - upstream batch update forwards matching members only",
            "[runtime][unit][manual-list][update]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});
    source.addInitial(TrackId{2});
    source.addInitial(TrackId{3});
    source.addInitial(TrackId{4});

    auto lv = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
    auto mls = ManualListSource{lv.view(), &source};

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    auto const batch = std::array{TrackId{1}, TrackId{2}, TrackId{4}, TrackId{5}};
    source.batchUpdate(batch);

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::BatchUpdated);
    CHECK(spy.events[0].batchIds.size() == 2);
    CHECK(std::ranges::contains(spy.events[0].batchIds, TrackId{1}));
    CHECK(std::ranges::contains(spy.events[0].batchIds, TrackId{2}));

    mls.detach(&spy);
  }

  TEST_CASE("ManualListSource - upstream batch update ignores batches without matching members",
            "[runtime][unit][manual-list][update]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});

    auto lv = ListViewOwner{{TrackId{1}}};
    auto mls = ManualListSource{lv.view(), &source};

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    auto const batch = std::array{TrackId{99}, TrackId{100}};
    source.batchUpdate(batch);

    CHECK(spy.events.empty());

    mls.detach(&spy);
  }

  TEST_CASE("ManualListSource - upstream batch update ignores empty batches", "[runtime][unit][manual-list][update]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});

    auto lv = ListViewOwner{{TrackId{1}}};
    auto mls = ManualListSource{lv.view(), &source};

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    source.batchUpdate({});

    CHECK(spy.events.empty());

    mls.detach(&spy);
  }

  TEST_CASE("ManualListSource - upstream batch update forwards all matching IDs",
            "[runtime][unit][manual-list][update]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});
    source.addInitial(TrackId{2});
    source.addInitial(TrackId{3});

    auto lv = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
    auto mls = ManualListSource{lv.view(), &source};

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    auto const batch = std::array{TrackId{1}, TrackId{2}, TrackId{3}};
    source.batchUpdate(batch);

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::BatchUpdated);
    CHECK(spy.events[0].batchIds.size() == 3);
    CHECK(std::ranges::contains(spy.events[0].batchIds, TrackId{1}));
    CHECK(std::ranges::contains(spy.events[0].batchIds, TrackId{2}));
    CHECK(std::ranges::contains(spy.events[0].batchIds, TrackId{3}));

    mls.detach(&spy);
  }

  // =============================================================================
  // Removal forwarding
  // =============================================================================
  TEST_CASE("ManualListSource - upstream removal deletes member and reports local index",
            "[runtime][unit][manual-list][remove]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{10});
    source.addInitial(TrackId{20});
    source.addInitial(TrackId{30});

    auto lv = ListViewOwner{{TrackId{10}, TrackId{20}, TrackId{30}}};
    auto mls = ManualListSource{lv.view(), &source};

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    source.remove(TrackId{20});

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::Removed);
    CHECK(spy.events[0].id == TrackId{20});
    CHECK(spy.events[0].index == 1);
    REQUIRE(mls.size() == 2);
    CHECK(mls.trackIdAt(0) == TrackId{10});
    CHECK(mls.trackIdAt(1) == TrackId{30});

    mls.detach(&spy);
  }

  TEST_CASE("ManualListSource - upstream removal reports first element index before erasure",
            "[runtime][unit][manual-list][remove]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});
    source.addInitial(TrackId{2});

    auto lv = ListViewOwner{{TrackId{1}, TrackId{2}}};
    auto mls = ManualListSource{lv.view(), &source};

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    source.remove(TrackId{1});

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::Removed);
    CHECK(spy.events[0].id == TrackId{1});
    CHECK(spy.events[0].index == 0);
    REQUIRE(mls.size() == 1);
    CHECK(mls.trackIdAt(0) == TrackId{2});

    mls.detach(&spy);
  }

  TEST_CASE("ManualListSource - upstream removal ignores non-member tracks", "[runtime][unit][manual-list][remove]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});
    source.addInitial(TrackId{99});

    auto lv = ListViewOwner{{TrackId{1}}};
    auto mls = ManualListSource{lv.view(), &source};

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    source.remove(TrackId{99});

    CHECK(spy.events.empty());
    CHECK(mls.size() == 1);
    CHECK(mls.trackIdAt(0) == TrackId{1});

    mls.detach(&spy);
  }

  TEST_CASE("ManualListSource - upstream batch removal deletes matching members and notifies observers",
            "[runtime][unit][manual-list][remove]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});
    source.addInitial(TrackId{2});
    source.addInitial(TrackId{3});
    source.addInitial(TrackId{4});
    source.addInitial(TrackId{5});

    auto lv = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}, TrackId{4}}};
    auto mls = ManualListSource{lv.view(), &source};

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    auto const batch = std::array{TrackId{2}, TrackId{5}, TrackId{4}};
    source.batchRemove(batch);

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::BatchRemoved);
    CHECK(spy.events[0].batchIds.size() == 2);
    CHECK(std::ranges::contains(spy.events[0].batchIds, TrackId{2}));
    CHECK(std::ranges::contains(spy.events[0].batchIds, TrackId{4}));
    REQUIRE(mls.size() == 2);
    CHECK(mls.trackIdAt(0) == TrackId{1});
    CHECK(mls.trackIdAt(1) == TrackId{3});

    mls.detach(&spy);
  }

  TEST_CASE("ManualListSource - upstream batch removal ignores batches without matching members",
            "[runtime][unit][manual-list][remove]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});

    auto lv = ListViewOwner{{TrackId{1}}};
    auto mls = ManualListSource{lv.view(), &source};

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    auto const batch = std::array{TrackId{99}, TrackId{100}};
    source.batchRemove(batch);

    CHECK(spy.events.empty());
    CHECK(mls.size() == 1);

    mls.detach(&spy);
  }

  TEST_CASE("ManualListSource - upstream batch removal ignores empty batches", "[runtime][unit][manual-list][remove]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});

    auto lv = ListViewOwner{{TrackId{1}}};
    auto mls = ManualListSource{lv.view(), &source};

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    source.batchRemove({});

    CHECK(spy.events.empty());

    mls.detach(&spy);
  }

  TEST_CASE("ManualListSource - upstream batch removal deletes all matching IDs",
            "[runtime][unit][manual-list][remove]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});
    source.addInitial(TrackId{2});
    source.addInitial(TrackId{3});

    auto lv = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
    auto mls = ManualListSource{lv.view(), &source};

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    auto const batch = std::array{TrackId{1}, TrackId{2}, TrackId{3}};
    source.batchRemove(batch);

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::BatchRemoved);
    CHECK(spy.events[0].batchIds.size() == 3);
    CHECK(std::ranges::contains(spy.events[0].batchIds, TrackId{1}));
    CHECK(std::ranges::contains(spy.events[0].batchIds, TrackId{2}));
    CHECK(std::ranges::contains(spy.events[0].batchIds, TrackId{3}));
    CHECK(mls.size() == 0);

    mls.detach(&spy);
  }

  // =============================================================================
  // Sequential removals
  // =============================================================================
  TEST_CASE("ManualListSource - sequential removals maintain correct indices", "[runtime][unit][manual-list][remove]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});
    source.addInitial(TrackId{2});
    source.addInitial(TrackId{3});

    auto lv = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
    auto mls = ManualListSource{lv.view(), &source};

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    // Remove middle element first.
    source.remove(TrackId{2});

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].id == TrackId{2});
    CHECK(spy.events[0].index == 1);
    REQUIRE(mls.size() == 2);
    CHECK(mls.trackIdAt(0) == TrackId{1});
    CHECK(mls.trackIdAt(1) == TrackId{3});

    // Remove first element.
    spy.clear();
    source.remove(TrackId{1});

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].id == TrackId{1});
    CHECK(spy.events[0].index == 0);
    REQUIRE(mls.size() == 1);
    CHECK(mls.trackIdAt(0) == TrackId{3});

    // Remove last element.
    spy.clear();
    source.remove(TrackId{3});

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].id == TrackId{3});
    CHECK(spy.events[0].index == 0);
    CHECK(mls.size() == 0);

    mls.detach(&spy);
  }

  // =============================================================================
  // Batch then single operations
  // =============================================================================
  TEST_CASE("ManualListSource - batch removal leaves later single removal indexed correctly",
            "[runtime][unit][manual-list][batch]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});
    source.addInitial(TrackId{2});
    source.addInitial(TrackId{3});
    source.addInitial(TrackId{4});

    auto lv = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}, TrackId{4}}};
    auto mls = ManualListSource{lv.view(), &source};

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    auto const batch = std::array{TrackId{1}, TrackId{2}};
    source.batchRemove(batch);

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::BatchRemoved);
    CHECK(spy.events[0].batchIds.size() == 2);
    REQUIRE(mls.size() == 2);

    spy.clear();
    source.remove(TrackId{3});

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::Removed);
    CHECK(spy.events[0].id == TrackId{3});
    CHECK(spy.events[0].index == 0);
    REQUIRE(mls.size() == 1);
    CHECK(mls.trackIdAt(0) == TrackId{4});

    mls.detach(&spy);
  }

  TEST_CASE("ManualListSource - batch update leaves later single update indexed correctly",
            "[runtime][unit][manual-list][batch]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});
    source.addInitial(TrackId{2});
    source.addInitial(TrackId{3});

    auto lv = ListViewOwner{{TrackId{1}, TrackId{2}}};
    auto mls = ManualListSource{lv.view(), &source};

    auto spy = TrackSourceObserverSpy{};
    mls.attach(&spy);

    auto const batch = std::array{TrackId{1}, TrackId{2}};
    source.batchUpdate(batch);

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::BatchUpdated);

    spy.clear();
    source.update(TrackId{1});

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::Updated);
    CHECK(spy.events[0].id == TrackId{1});

    mls.detach(&spy);
  }

  // =============================================================================
  // Destruction
  // =============================================================================
  TEST_CASE("ManualListSource - destructor detaches from upstream source", "[runtime][unit][manual-list][lifetime]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});

    {
      auto lv = ListViewOwner{{TrackId{1}}};
      auto mls = ManualListSource{lv.view(), &source};
      CHECK(mls.source() == &source);
    }

    // Triggering events on source after ManualListSource destruction
    // must not crash (no dangling observer pointer).
    source.insert(TrackId{2}, 1);
    source.update(TrackId{1});
    source.remove(TrackId{1});
    source.reset({{TrackId{3}}});
  }

  // =============================================================================
  // Multiple observers
  // =============================================================================
  TEST_CASE("ManualListSource - attached observers receive events until detached",
            "[runtime][unit][manual-list][observer]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});
    source.addInitial(TrackId{2});

    auto lv = ListViewOwner{{TrackId{1}, TrackId{2}}};
    auto mls = ManualListSource{lv.view(), &source};

    auto spy1 = TrackSourceObserverSpy{};
    auto spy2 = TrackSourceObserverSpy{};
    mls.attach(&spy1);
    mls.attach(&spy2);

    source.update(TrackId{1});

    REQUIRE(spy1.events.size() == 1);
    CHECK(spy1.events[0].kind == TrackSourceObserverSpy::EventKind::Updated);
    CHECK(spy1.events[0].id == TrackId{1});

    REQUIRE(spy2.events.size() == 1);
    CHECK(spy2.events[0].kind == TrackSourceObserverSpy::EventKind::Updated);
    CHECK(spy2.events[0].id == TrackId{1});

    mls.detach(&spy1);
    mls.detach(&spy2);
  }

  TEST_CASE("ManualListSource - detached observers no longer receive events", "[runtime][unit][manual-list][observer]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});

    auto lv = ListViewOwner{{TrackId{1}}};
    auto mls = ManualListSource{lv.view(), &source};

    auto spy1 = TrackSourceObserverSpy{};
    auto spy2 = TrackSourceObserverSpy{};
    mls.attach(&spy1);
    mls.attach(&spy2);
    mls.detach(&spy2);

    source.update(TrackId{1});

    REQUIRE(spy1.events.size() == 1);
    CHECK(spy2.events.empty());

    mls.detach(&spy1);
  }

  // =============================================================================
  // Chained ManualListSources
  // =============================================================================
  TEST_CASE("ManualListSource - chained manual lists propagate upstream removals",
            "[runtime][unit][manual-list][chain]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});
    source.addInitial(TrackId{2});
    source.addInitial(TrackId{3});

    auto lv1 = ListViewOwner{{TrackId{1}, TrackId{2}}};
    auto inner = ManualListSource{lv1.view(), &source};

    auto lv2 = ListViewOwner{{TrackId{2}}};
    auto outer = ManualListSource{lv2.view(), &inner};

    auto outerSpy = TrackSourceObserverSpy{};
    outer.attach(&outerSpy);

    source.remove(TrackId{2});

    REQUIRE(inner.size() == 1);
    CHECK(inner.trackIdAt(0) == TrackId{1});
    REQUIRE(outer.size() == 0);

    REQUIRE(outerSpy.events.size() == 1);
    CHECK(outerSpy.events[0].kind == TrackSourceObserverSpy::EventKind::Removed);
    CHECK(outerSpy.events[0].id == TrackId{2});
    CHECK(outerSpy.events[0].index == 0);

    outer.detach(&outerSpy);
  }

  TEST_CASE("ManualListSource - chained manual lists propagate upstream resets", "[runtime][unit][manual-list][chain]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});
    source.addInitial(TrackId{2});
    source.addInitial(TrackId{3});

    auto lv1 = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
    auto inner = ManualListSource{lv1.view(), &source};

    auto lv2 = ListViewOwner{{TrackId{1}, TrackId{3}}};
    auto outer = ManualListSource{lv2.view(), &inner};

    auto outerSpy = TrackSourceObserverSpy{};
    outer.attach(&outerSpy);

    source.reset({{TrackId{3}}});

    REQUIRE(inner.size() == 1);
    CHECK(inner.trackIdAt(0) == TrackId{3});
    REQUIRE(outer.size() == 1);
    CHECK(outer.trackIdAt(0) == TrackId{3});

    REQUIRE(!outerSpy.events.empty());
    CHECK(outerSpy.events.back().kind == TrackSourceObserverSpy::EventKind::Reset);

    outer.detach(&outerSpy);
  }

  // =============================================================================
  // Destructor with null source
  // =============================================================================
  TEST_CASE("ManualListSource - destructor is safe without an upstream source",
            "[runtime][unit][manual-list][lifetime]")
  {
    {
      auto lv = ListViewOwner{{TrackId{1}, TrackId{2}}};
      auto mls = ManualListSource{lv.view()};
      CHECK(mls.source() == nullptr);
    }

    // mls destroyed - detach is skipped when _source is nullptr.
    CHECK(true);
  }
} // namespace ao::rt::test
