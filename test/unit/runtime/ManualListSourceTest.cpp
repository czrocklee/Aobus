// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include <ao/Type.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListView.h>
#include <runtime/ManualListSource.h>
#include <runtime/TrackSource.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    class MutableTrackSource final : public TrackSource
    {
    public:
      void addInitial(TrackId id) { _ids.push_back(id); }

      void insert(TrackId id, std::size_t index)
      {
        _ids.insert(_ids.begin() + static_cast<std::ptrdiff_t>(index), id);
        notifyInserted(id, index);
      }

      void update(TrackId id)
      {
        auto const idx = indexOf(id);
        REQUIRE(idx.has_value());
        notifyUpdated(id, *idx);
      }

      void remove(TrackId id)
      {
        auto const idx = indexOf(id);
        REQUIRE(idx.has_value());
        _ids.erase(_ids.begin() + static_cast<std::ptrdiff_t>(*idx));
        notifyRemoved(id, *idx);
      }

      void resetTo(std::vector<TrackId> ids)
      {
        _ids = std::move(ids);
        notifyReset();
      }

      void batchInsert(std::span<TrackId const> ids)
      {
        _ids.insert(_ids.end(), ids.begin(), ids.end());
        notifyInserted(ids);
      }

      void batchUpdate(std::span<TrackId const> ids) { notifyUpdated(ids); }

      void batchRemove(std::span<TrackId const> ids)
      {
        auto removed = std::vector<TrackId>{};

        for (auto id : ids)
        {
          if (auto it = std::ranges::find(_ids, id); it != _ids.end())
          {
            _ids.erase(it);
            removed.push_back(id);
          }
        }

        notifyRemoved(removed);
      }

      std::size_t size() const override { return _ids.size(); }
      TrackId trackIdAt(std::size_t index) const override { return _ids.at(index); }

      std::optional<std::size_t> indexOf(TrackId id) const override
      {
        if (auto it = std::ranges::find(_ids, id); it != _ids.end())
        {
          return static_cast<std::size_t>(std::ranges::distance(_ids.begin(), it));
        }

        return std::nullopt;
      }

    private:
      std::vector<TrackId> _ids;
    };

    class ObserverSpy final : public TrackSourceObserver
    {
    public:
      enum class EventKind : std::uint8_t
      {
        Reset,
        Inserted,
        Updated,
        Removed,
        BatchInserted,
        BatchUpdated,
        BatchRemoved,
      };

      struct Event final
      {
        EventKind kind;
        TrackId id{};
        std::size_t index = 0;
        std::vector<TrackId> batchIds{};
      };

      void onReset() override { events.push_back({.kind = EventKind::Reset}); }

      void onInserted(TrackId id, std::size_t index) override
      {
        events.push_back({.kind = EventKind::Inserted, .id = id, .index = index});
      }

      void onUpdated(TrackId id, std::size_t index) override
      {
        events.push_back({.kind = EventKind::Updated, .id = id, .index = index});
      }

      void onRemoved(TrackId id, std::size_t index) override
      {
        events.push_back({.kind = EventKind::Removed, .id = id, .index = index});
      }

      void onInserted(std::span<TrackId const> ids) override
      {
        events.push_back({.kind = EventKind::BatchInserted, .batchIds = {ids.begin(), ids.end()}});
      }

      void onUpdated(std::span<TrackId const> ids) override
      {
        events.push_back({.kind = EventKind::BatchUpdated, .batchIds = {ids.begin(), ids.end()}});
      }

      void onRemoved(std::span<TrackId const> ids) override
      {
        events.push_back({.kind = EventKind::BatchRemoved, .batchIds = {ids.begin(), ids.end()}});
      }

      void clear() { events.clear(); }

      std::vector<Event> events;
    };

    // Helper that owns the serialized payload so the ListView span stays valid.
    class ListViewOwner final
    {
    public:
      explicit ListViewOwner(std::vector<TrackId> const& ids)
        : _payload{buildPayload(ids)}, _view{_payload}
      {
      }

      library::ListView const& view() const { return _view; }

    private:
      static std::vector<std::byte> buildPayload(std::vector<TrackId> const& ids)
      {
        auto builder = library::ListBuilder::createNew();

        for (auto id : ids)
        {
          builder.tracks().add(id);
        }

        return builder.serialize();
      }

      std::vector<std::byte> _payload;
      library::ListView _view;
    };
  } // namespace

  // =============================================================================
  // Construction
  // =============================================================================

  TEST_CASE("ManualListSource - default construction", "[app][manuallist]")
  {
    auto mls = ManualListSource{};

    CHECK(mls.size() == 0);
    CHECK(mls._trackIds.empty());
    CHECK(mls._source == nullptr);
  }

  TEST_CASE("ManualListSource - construction from ListView", "[app][manuallist]")
  {
    SECTION("copies all track IDs")
    {
      auto lv = ListViewOwner{{TrackId{10}, TrackId{20}, TrackId{30}}};
      auto mls = ManualListSource{lv.view()};

      REQUIRE(mls.size() == 3);
      CHECK(mls.trackIdAt(0) == TrackId{10});
      CHECK(mls.trackIdAt(1) == TrackId{20});
      CHECK(mls.trackIdAt(2) == TrackId{30});
      CHECK(mls._source == nullptr);
    }

    SECTION("empty ListView creates empty list")
    {
      auto lv = ListViewOwner{{}};
      auto mls = ManualListSource{lv.view()};

      CHECK(mls.size() == 0);
      CHECK(mls._trackIds.empty());
    }

    SECTION("with non-null source attaches as observer")
    {
      auto source = MutableTrackSource{};
      source.addInitial(TrackId{1});
      source.addInitial(TrackId{2});

      auto lv = ListViewOwner{{TrackId{1}}};
      auto mls = ManualListSource{lv.view(), &source};

      CHECK(mls._source == &source);
      CHECK(mls.size() == 1);
      CHECK(mls.trackIdAt(0) == TrackId{1});

      auto spy = ObserverSpy{};
      mls.attach(&spy);

      source.update(TrackId{1});

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::Updated);
      CHECK(spy.events[0].id == TrackId{1});

      mls.detach(&spy);
    }
  }

  // =============================================================================
  // TrackSource interface
  // =============================================================================

  TEST_CASE("ManualListSource - TrackSource interface", "[app][manuallist]")
  {
    SECTION("size returns number of tracks")
    {
      auto mls = ManualListSource{};

      CHECK(mls.size() == 0);

      mls._trackIds.push_back(TrackId{1});
      CHECK(mls.size() == 1);

      mls._trackIds.push_back(TrackId{2});
      mls._trackIds.push_back(TrackId{3});
      CHECK(mls.size() == 3);
    }

    SECTION("trackIdAt returns correct ID at each index")
    {
      auto lv = ListViewOwner{{TrackId{100}, TrackId{200}, TrackId{300}}};
      auto mls = ManualListSource{lv.view()};

      CHECK(mls.trackIdAt(0) == TrackId{100});
      CHECK(mls.trackIdAt(1) == TrackId{200});
      CHECK(mls.trackIdAt(2) == TrackId{300});
    }

    SECTION("indexOf returns correct index for member")
    {
      auto lv = ListViewOwner{{TrackId{5}, TrackId{10}, TrackId{15}}};
      auto mls = ManualListSource{lv.view()};

      CHECK(mls.indexOf(TrackId{5}) == std::optional{std::size_t{0}});
      CHECK(mls.indexOf(TrackId{10}) == std::optional{std::size_t{1}});
      CHECK(mls.indexOf(TrackId{15}) == std::optional{std::size_t{2}});
    }

    SECTION("indexOf returns nullopt for non-member and empty list")
    {
      auto lv = ListViewOwner{{TrackId{1}, TrackId{2}}};
      auto mls = ManualListSource{lv.view()};

      CHECK(mls.indexOf(TrackId{99}) == std::nullopt);

      auto emptyMls = ManualListSource{};
      CHECK(emptyMls.indexOf(TrackId{1}) == std::nullopt);
    }

    SECTION("contains returns true for member, false for non-member")
    {
      auto lv = ListViewOwner{{TrackId{42}, TrackId{43}}};
      auto mls = ManualListSource{lv.view()};

      CHECK(mls.contains(TrackId{42}));
      CHECK(mls.contains(TrackId{43}));
      CHECK_FALSE(mls.contains(TrackId{1}));
      CHECK_FALSE(mls.contains(TrackId{99}));
    }
  }

  // =============================================================================
  // reloadFromListView
  // =============================================================================

  TEST_CASE("ManualListSource - reloadFromListView", "[app][manuallist]")
  {
    SECTION("replaces all tracks when no source")
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

    SECTION("filters tracks against source")
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

    SECTION("emits notifyReset to own observers")
    {
      auto lv1 = ListViewOwner{{TrackId{7}, TrackId{8}}};
      auto mls = ManualListSource{lv1.view()};

      auto spy = ObserverSpy{};
      mls.attach(&spy);

      auto lv2 = ListViewOwner{{TrackId{9}}};
      mls.reloadFromListView(lv2.view());

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::Reset);
      REQUIRE(mls.size() == 1);
      CHECK(mls.trackIdAt(0) == TrackId{9});

      mls.detach(&spy);
    }

    SECTION("empty view clears list and emits reset")
    {
      auto lv = ListViewOwner{{TrackId{1}, TrackId{2}}};
      auto mls = ManualListSource{lv.view()};

      auto spy = ObserverSpy{};
      mls.attach(&spy);

      auto emptyLv = ListViewOwner{{}};
      mls.reloadFromListView(emptyLv.view());

      CHECK(mls.size() == 0);
      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::Reset);

      mls.detach(&spy);
    }

    SECTION("with source keeps only tracks present in source")
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

    SECTION("with source filters all tracks when none are in source")
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
  }

  // =============================================================================
  // onReset observer
  // =============================================================================

  TEST_CASE("ManualListSource - onReset observer", "[app][manuallist]")
  {
    SECTION("no-ops when _source is null")
    {
      auto lv = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
      auto mls = ManualListSource{lv.view()};

      auto spy = ObserverSpy{};
      mls.attach(&spy);

      mls.onReset();

      CHECK(spy.events.empty());
      CHECK(mls.size() == 3);
      CHECK(mls._trackIds.size() == 3);

      mls.detach(&spy);
    }

    SECTION("filters stale IDs against current source and emits reset")
    {
      auto source = MutableTrackSource{};
      source.addInitial(TrackId{1});
      source.addInitial(TrackId{2});
      source.addInitial(TrackId{3});

      auto lv = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
      auto mls = ManualListSource{lv.view(), &source};

      mls._trackIds.push_back(TrackId{99});

      auto spy = ObserverSpy{};
      mls.attach(&spy);

      source.resetTo({TrackId{1}, TrackId{3}});

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::Reset);
      REQUIRE(mls.size() == 2);
      CHECK(mls.trackIdAt(0) == TrackId{1});
      CHECK(mls.trackIdAt(1) == TrackId{3});

      mls.detach(&spy);
    }

    SECTION("clears all tracks when source becomes empty")
    {
      auto source = MutableTrackSource{};
      source.addInitial(TrackId{1});
      source.addInitial(TrackId{2});

      auto lv = ListViewOwner{{TrackId{1}, TrackId{2}}};
      auto mls = ManualListSource{lv.view(), &source};

      auto spy = ObserverSpy{};
      mls.attach(&spy);

      source.resetTo({});

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::Reset);
      CHECK(mls.size() == 0);
      CHECK(mls._trackIds.empty());

      mls.detach(&spy);
    }

    SECTION("keeps all tracks when all are still in source")
    {
      auto source = MutableTrackSource{};
      source.addInitial(TrackId{1});
      source.addInitial(TrackId{2});
      source.addInitial(TrackId{3});

      auto lv = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
      auto mls = ManualListSource{lv.view(), &source};

      auto spy = ObserverSpy{};
      mls.attach(&spy);

      source.resetTo({TrackId{1}, TrackId{2}, TrackId{3}});

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::Reset);
      REQUIRE(mls.size() == 3);
      CHECK(mls.trackIdAt(0) == TrackId{1});
      CHECK(mls.trackIdAt(1) == TrackId{2});
      CHECK(mls.trackIdAt(2) == TrackId{3});

      mls.detach(&spy);
    }
  }

  // =============================================================================

  TEST_CASE("ManualListSource - onInserted is no-op", "[app][manuallist]")
  {
    SECTION("single item")
    {
      auto lv = ListViewOwner{{TrackId{1}, TrackId{2}}};
      auto mls = ManualListSource{lv.view()};

      auto spy = ObserverSpy{};
      mls.attach(&spy);

      mls.onInserted(TrackId{99}, 0);

      CHECK(spy.events.empty());
      CHECK(mls.size() == 2);
      CHECK_FALSE(mls.contains(TrackId{99}));

      mls.detach(&spy);
    }

    SECTION("batch")
    {
      auto lv = ListViewOwner{{TrackId{1}}};
      auto mls = ManualListSource{lv.view()};

      auto spy = ObserverSpy{};
      mls.attach(&spy);

      TrackId batch[] = {TrackId{10}, TrackId{20}};
      mls.onInserted(std::span{batch});

      CHECK(spy.events.empty());
      CHECK(mls.size() == 1);

      mls.detach(&spy);
    }
  }

  // =============================================================================
  // onUpdated observer
  // =============================================================================

  TEST_CASE("ManualListSource - onUpdated observer", "[app][manuallist]")
  {
    SECTION("single re-emits for member track with correct local index")
    {
      auto source = MutableTrackSource{};
      source.addInitial(TrackId{1});
      source.addInitial(TrackId{2});
      source.addInitial(TrackId{3});

      auto lv = ListViewOwner{{TrackId{1}, TrackId{3}}};
      auto mls = ManualListSource{lv.view(), &source};

      auto spy = ObserverSpy{};
      mls.attach(&spy);

      source.update(TrackId{3});

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::Updated);
      CHECK(spy.events[0].id == TrackId{3});
      CHECK(spy.events[0].index == 1);

      spy.clear();
      source.update(TrackId{1});

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::Updated);
      CHECK(spy.events[0].id == TrackId{1});
      CHECK(spy.events[0].index == 0);

      mls.detach(&spy);
    }

    SECTION("single no-ops silently for non-member track")
    {
      auto source = MutableTrackSource{};
      source.addInitial(TrackId{1});
      source.addInitial(TrackId{99});

      auto lv = ListViewOwner{{TrackId{1}}};
      auto mls = ManualListSource{lv.view(), &source};

      auto spy = ObserverSpy{};
      mls.attach(&spy);

      source.update(TrackId{99});

      CHECK(spy.events.empty());
      CHECK(mls.size() == 1);

      mls.detach(&spy);
    }

    SECTION("batch emits for matching members only")
    {
      auto source = MutableTrackSource{};
      source.addInitial(TrackId{1});
      source.addInitial(TrackId{2});
      source.addInitial(TrackId{3});
      source.addInitial(TrackId{4});

      auto lv = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
      auto mls = ManualListSource{lv.view(), &source};

      auto spy = ObserverSpy{};
      mls.attach(&spy);

      TrackId batch[] = {TrackId{1}, TrackId{2}, TrackId{4}, TrackId{5}};
      source.batchUpdate(batch);

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::BatchUpdated);
      REQUIRE(spy.events[0].batchIds.size() == 2);
      CHECK(std::ranges::contains(spy.events[0].batchIds, TrackId{1}));
      CHECK(std::ranges::contains(spy.events[0].batchIds, TrackId{2}));

      mls.detach(&spy);
    }

    SECTION("batch no-ops when no members match")
    {
      auto source = MutableTrackSource{};
      source.addInitial(TrackId{1});

      auto lv = ListViewOwner{{TrackId{1}}};
      auto mls = ManualListSource{lv.view(), &source};

      auto spy = ObserverSpy{};
      mls.attach(&spy);

      TrackId batch[] = {TrackId{99}, TrackId{100}};
      source.batchUpdate(batch);

      CHECK(spy.events.empty());

      mls.detach(&spy);
    }

    SECTION("batch no-ops when batch is empty")
    {
      auto source = MutableTrackSource{};
      source.addInitial(TrackId{1});

      auto lv = ListViewOwner{{TrackId{1}}};
      auto mls = ManualListSource{lv.view(), &source};

      auto spy = ObserverSpy{};
      mls.attach(&spy);

      source.batchUpdate({});

      CHECK(spy.events.empty());

      mls.detach(&spy);
    }

    SECTION("batch emits when all IDs match")
    {
      auto source = MutableTrackSource{};
      source.addInitial(TrackId{1});
      source.addInitial(TrackId{2});
      source.addInitial(TrackId{3});

      auto lv = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
      auto mls = ManualListSource{lv.view(), &source};

      auto spy = ObserverSpy{};
      mls.attach(&spy);

      TrackId batch[] = {TrackId{1}, TrackId{2}, TrackId{3}};
      source.batchUpdate(batch);

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::BatchUpdated);
      REQUIRE(spy.events[0].batchIds.size() == 3);
      CHECK(std::ranges::contains(spy.events[0].batchIds, TrackId{1}));
      CHECK(std::ranges::contains(spy.events[0].batchIds, TrackId{2}));
      CHECK(std::ranges::contains(spy.events[0].batchIds, TrackId{3}));

      mls.detach(&spy);
    }
  }

  // =============================================================================
  // onRemoved observer
  // =============================================================================

  TEST_CASE("ManualListSource - onRemoved observer", "[app][manuallist]")
  {
    SECTION("single removes member and emits notification with correct index")
    {
      auto source = MutableTrackSource{};
      source.addInitial(TrackId{10});
      source.addInitial(TrackId{20});
      source.addInitial(TrackId{30});

      auto lv = ListViewOwner{{TrackId{10}, TrackId{20}, TrackId{30}}};
      auto mls = ManualListSource{lv.view(), &source};

      auto spy = ObserverSpy{};
      mls.attach(&spy);

      source.remove(TrackId{20});

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::Removed);
      CHECK(spy.events[0].id == TrackId{20});
      CHECK(spy.events[0].index == 1);
      REQUIRE(mls.size() == 2);
      CHECK(mls.trackIdAt(0) == TrackId{10});
      CHECK(mls.trackIdAt(1) == TrackId{30});

      mls.detach(&spy);
    }

    SECTION("reports index before erasure for first element")
    {
      auto source = MutableTrackSource{};
      source.addInitial(TrackId{1});
      source.addInitial(TrackId{2});

      auto lv = ListViewOwner{{TrackId{1}, TrackId{2}}};
      auto mls = ManualListSource{lv.view(), &source};

      auto spy = ObserverSpy{};
      mls.attach(&spy);

      source.remove(TrackId{1});

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::Removed);
      CHECK(spy.events[0].id == TrackId{1});
      CHECK(spy.events[0].index == 0);
      REQUIRE(mls.size() == 1);
      CHECK(mls.trackIdAt(0) == TrackId{2});

      mls.detach(&spy);
    }

    SECTION("single no-ops for non-member track")
    {
      auto source = MutableTrackSource{};
      source.addInitial(TrackId{1});
      source.addInitial(TrackId{99});

      auto lv = ListViewOwner{{TrackId{1}}};
      auto mls = ManualListSource{lv.view(), &source};

      auto spy = ObserverSpy{};
      mls.attach(&spy);

      source.remove(TrackId{99});

      CHECK(spy.events.empty());
      CHECK(mls.size() == 1);
      CHECK(mls.trackIdAt(0) == TrackId{1});

      mls.detach(&spy);
    }

    SECTION("batch removes matching members and emits batch notification")
    {
      auto source = MutableTrackSource{};
      source.addInitial(TrackId{1});
      source.addInitial(TrackId{2});
      source.addInitial(TrackId{3});
      source.addInitial(TrackId{4});
      source.addInitial(TrackId{5});

      auto lv = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}, TrackId{4}}};
      auto mls = ManualListSource{lv.view(), &source};

      auto spy = ObserverSpy{};
      mls.attach(&spy);

      TrackId batch[] = {TrackId{2}, TrackId{5}, TrackId{4}};
      source.batchRemove(batch);

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::BatchRemoved);
      REQUIRE(spy.events[0].batchIds.size() == 2);
      CHECK(std::ranges::contains(spy.events[0].batchIds, TrackId{2}));
      CHECK(std::ranges::contains(spy.events[0].batchIds, TrackId{4}));
      REQUIRE(mls.size() == 2);
      CHECK(mls.trackIdAt(0) == TrackId{1});
      CHECK(mls.trackIdAt(1) == TrackId{3});

      mls.detach(&spy);
    }

    SECTION("batch no-ops when no members match")
    {
      auto source = MutableTrackSource{};
      source.addInitial(TrackId{1});

      auto lv = ListViewOwner{{TrackId{1}}};
      auto mls = ManualListSource{lv.view(), &source};

      auto spy = ObserverSpy{};
      mls.attach(&spy);

      TrackId batch[] = {TrackId{99}, TrackId{100}};
      source.batchRemove(batch);

      CHECK(spy.events.empty());
      CHECK(mls.size() == 1);

      mls.detach(&spy);
    }

    SECTION("batch no-ops when batch is empty")
    {
      auto source = MutableTrackSource{};
      source.addInitial(TrackId{1});

      auto lv = ListViewOwner{{TrackId{1}}};
      auto mls = ManualListSource{lv.view(), &source};

      auto spy = ObserverSpy{};
      mls.attach(&spy);

      source.batchRemove({});

      CHECK(spy.events.empty());

      mls.detach(&spy);
    }

    SECTION("batch removes when all IDs match")
    {
      auto source = MutableTrackSource{};
      source.addInitial(TrackId{1});
      source.addInitial(TrackId{2});
      source.addInitial(TrackId{3});

      auto lv = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
      auto mls = ManualListSource{lv.view(), &source};

      auto spy = ObserverSpy{};
      mls.attach(&spy);

      TrackId batch[] = {TrackId{1}, TrackId{2}, TrackId{3}};
      source.batchRemove(batch);

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::BatchRemoved);
      REQUIRE(spy.events[0].batchIds.size() == 3);
      CHECK(std::ranges::contains(spy.events[0].batchIds, TrackId{1}));
      CHECK(std::ranges::contains(spy.events[0].batchIds, TrackId{2}));
      CHECK(std::ranges::contains(spy.events[0].batchIds, TrackId{3}));
      CHECK(mls.size() == 0);

      mls.detach(&spy);
    }
  }

  // =============================================================================
  // Sequential removals
  // =============================================================================

  TEST_CASE("ManualListSource - sequential removals maintain correct indices", "[app][manuallist]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});
    source.addInitial(TrackId{2});
    source.addInitial(TrackId{3});

    auto lv = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
    auto mls = ManualListSource{lv.view(), &source};

    auto spy = ObserverSpy{};
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

  TEST_CASE("ManualListSource - batch then single operations", "[app][manuallist]")
  {
    SECTION("batch removal followed by single removal")
    {
      auto source = MutableTrackSource{};
      source.addInitial(TrackId{1});
      source.addInitial(TrackId{2});
      source.addInitial(TrackId{3});
      source.addInitial(TrackId{4});

      auto lv = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}, TrackId{4}}};
      auto mls = ManualListSource{lv.view(), &source};

      auto spy = ObserverSpy{};
      mls.attach(&spy);

      TrackId batch[] = {TrackId{1}, TrackId{2}};
      source.batchRemove(batch);

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::BatchRemoved);
      CHECK(spy.events[0].batchIds.size() == 2);
      REQUIRE(mls.size() == 2);

      spy.clear();
      source.remove(TrackId{3});

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::Removed);
      CHECK(spy.events[0].id == TrackId{3});
      CHECK(spy.events[0].index == 0);
      REQUIRE(mls.size() == 1);
      CHECK(mls.trackIdAt(0) == TrackId{4});

      mls.detach(&spy);
    }

    SECTION("batch update followed by single update")
    {
      auto source = MutableTrackSource{};
      source.addInitial(TrackId{1});
      source.addInitial(TrackId{2});
      source.addInitial(TrackId{3});

      auto lv = ListViewOwner{{TrackId{1}, TrackId{2}}};
      auto mls = ManualListSource{lv.view(), &source};

      auto spy = ObserverSpy{};
      mls.attach(&spy);

      TrackId batch[] = {TrackId{1}, TrackId{2}};
      source.batchUpdate(batch);

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::BatchUpdated);

      spy.clear();
      source.update(TrackId{1});

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::Updated);
      CHECK(spy.events[0].id == TrackId{1});

      mls.detach(&spy);
    }
  }

  // =============================================================================
  // Destruction
  // =============================================================================

  TEST_CASE("ManualListSource - destructor detaches from source", "[app][manuallist]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{1});

    {
      auto lv = ListViewOwner{{TrackId{1}}};
      auto mls = ManualListSource{lv.view(), &source};
      CHECK(mls._source == &source);
    }

    // Triggering events on source after ManualListSource destruction
    // must not crash (no dangling observer pointer).
    source.insert(TrackId{2}, 1);
    source.update(TrackId{1});
    source.remove(TrackId{1});
    source.resetTo({TrackId{3}});
  }

  // =============================================================================
  // Multiple observers
  // =============================================================================

  TEST_CASE("ManualListSource - multiple observers", "[app][manuallist]")
  {
    SECTION("all attached observers receive relayed events")
    {
      auto source = MutableTrackSource{};
      source.addInitial(TrackId{1});
      source.addInitial(TrackId{2});

      auto lv = ListViewOwner{{TrackId{1}, TrackId{2}}};
      auto mls = ManualListSource{lv.view(), &source};

      auto spy1 = ObserverSpy{};
      auto spy2 = ObserverSpy{};
      mls.attach(&spy1);
      mls.attach(&spy2);

      source.update(TrackId{1});

      REQUIRE(spy1.events.size() == 1);
      CHECK(spy1.events[0].kind == ObserverSpy::EventKind::Updated);
      CHECK(spy1.events[0].id == TrackId{1});

      REQUIRE(spy2.events.size() == 1);
      CHECK(spy2.events[0].kind == ObserverSpy::EventKind::Updated);
      CHECK(spy2.events[0].id == TrackId{1});

      mls.detach(&spy1);
      mls.detach(&spy2);
    }

    SECTION("detached observer no longer receives events")
    {
      auto source = MutableTrackSource{};
      source.addInitial(TrackId{1});

      auto lv = ListViewOwner{{TrackId{1}}};
      auto mls = ManualListSource{lv.view(), &source};

      auto spy1 = ObserverSpy{};
      auto spy2 = ObserverSpy{};
      mls.attach(&spy1);
      mls.attach(&spy2);
      mls.detach(&spy2);

      source.update(TrackId{1});

      REQUIRE(spy1.events.size() == 1);
      CHECK(spy2.events.empty());

      mls.detach(&spy1);
    }
  }

  // =============================================================================
  // Chained ManualListSources
  // =============================================================================

  TEST_CASE("ManualListSource - chained lists", "[app][manuallist]")
  {
    SECTION("removal propagates through chain")
    {
      auto source = MutableTrackSource{};
      source.addInitial(TrackId{1});
      source.addInitial(TrackId{2});
      source.addInitial(TrackId{3});

      auto lv1 = ListViewOwner{{TrackId{1}, TrackId{2}}};
      auto inner = ManualListSource{lv1.view(), &source};

      auto lv2 = ListViewOwner{{TrackId{2}}};
      auto outer = ManualListSource{lv2.view(), &inner};

      auto outerSpy = ObserverSpy{};
      outer.attach(&outerSpy);

      source.remove(TrackId{2});

      REQUIRE(inner.size() == 1);
      CHECK(inner.trackIdAt(0) == TrackId{1});
      REQUIRE(outer.size() == 0);

      REQUIRE(outerSpy.events.size() == 1);
      CHECK(outerSpy.events[0].kind == ObserverSpy::EventKind::Removed);
      CHECK(outerSpy.events[0].id == TrackId{2});
      CHECK(outerSpy.events[0].index == 0);

      outer.detach(&outerSpy);
    }

    SECTION("reset propagates through chain")
    {
      auto source = MutableTrackSource{};
      source.addInitial(TrackId{1});
      source.addInitial(TrackId{2});
      source.addInitial(TrackId{3});

      auto lv1 = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
      auto inner = ManualListSource{lv1.view(), &source};

      auto lv2 = ListViewOwner{{TrackId{1}, TrackId{3}}};
      auto outer = ManualListSource{lv2.view(), &inner};

      auto outerSpy = ObserverSpy{};
      outer.attach(&outerSpy);

      source.resetTo({TrackId{3}});

      REQUIRE(inner.size() == 1);
      CHECK(inner.trackIdAt(0) == TrackId{3});
      REQUIRE(outer.size() == 1);
      CHECK(outer.trackIdAt(0) == TrackId{3});

      REQUIRE(outerSpy.events.size() >= 1);
      CHECK(outerSpy.events.back().kind == ObserverSpy::EventKind::Reset);

      outer.detach(&outerSpy);
    }
  }

  // =============================================================================
  // Destructor with null source
  // =============================================================================

  TEST_CASE("ManualListSource - destructor with null source does not crash", "[app][manuallist]")
  {
    {
      auto lv = ListViewOwner{{TrackId{1}, TrackId{2}}};
      auto mls = ManualListSource{lv.view()};
      CHECK(mls._source == nullptr);
    }

    // mls destroyed — detach is skipped when _source is nullptr.
    CHECK(true);
  }
} // namespace ao::rt::test
