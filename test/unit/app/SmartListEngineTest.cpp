// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/model/FilteredTrackIdList.h>
#include <ao/model/SmartListEngine.h>
#include <ao/model/TrackIdList.h>
#include <test/unit/lmdb/TestUtils.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
  using ao::TrackId;
  using ao::library::MusicLibrary;
  using ao::library::TrackBuilder;
  using ao::library::TrackStore;
  using ao::model::FilteredTrackIdList;
  using ao::model::SmartListEngine;
  using ao::model::TrackIdList;
  using ao::model::TrackIdListObserver;

  struct TrackSpec
  {
    std::string title = "Track";
    std::string artist = "Artist";
    std::uint16_t year = 0;
    std::uint16_t trackNumber = 0;
    std::uint32_t durationMs = 180000;
    std::string customKey;
    std::string customValue;
  };

  TrackSpec makeTrackSpec(std::string_view title, std::uint16_t year, std::uint32_t durationMs = 180000)
  {
    auto spec = TrackSpec{};
    spec.title = title;
    spec.year = year;
    spec.durationMs = durationMs;
    return spec;
  }

  class MutableTrackIdList final : public TrackIdList
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
      auto const index = indexOf(id);
      REQUIRE(index.has_value());
      notifyUpdated(id, *index);
    }

    void remove(TrackId id)
    {
      auto const index = indexOf(id);
      REQUIRE(index.has_value());
      _ids.erase(_ids.begin() + static_cast<std::ptrdiff_t>(*index));
      notifyRemoved(id, *index);
    }

    void batchInsert(std::span<TrackId const> ids)
    {
      _ids.insert(_ids.end(), ids.begin(), ids.end());
      notifyInserted(ids);
    }

    void batchUpdate(std::span<TrackId const> ids) { notifyUpdated(ids); }

    void batchRemove(std::span<TrackId const> ids)
    {
      std::vector<TrackId> actualRemoved;
      for (auto id : ids)
      {
        auto it = std::ranges::find(_ids, id);
        if (it != _ids.end())
        {
          _ids.erase(it);
          actualRemoved.push_back(id);
        }
      }
      notifyRemoved(actualRemoved);
    }

    std::size_t size() const override { return _ids.size(); }

    TrackId trackIdAt(std::size_t index) const override { return _ids.at(index); }

    std::optional<std::size_t> indexOf(TrackId id) const override
    {
      for (std::size_t i = 0; i < _ids.size(); ++i)
      {
        if (_ids[i] == id)
        {
          return i;
        }
      }

      return std::nullopt;
    }

  private:
    std::vector<TrackId> _ids;
  };

  class ObserverSpy final : public TrackIdListObserver
  {
  public:
    enum class EventKind
    {
      Reset,
      Inserted,
      Updated,
      Removed,
      BatchInserted,
      BatchUpdated,
      BatchRemoved,
    };

    struct Event
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

  class TestMusicLibrary final
  {
  public:
    TestMusicLibrary()
      : _tempDir{}, _library{_tempDir.path()}
    {
    }

    MusicLibrary& library() { return _library; }

    TrackId addTrack(TrackSpec const& spec)
    {
      auto txn = _library.writeTransaction();
      auto writer = _library.tracks().writer(txn);

      auto builder = TrackBuilder::createNew();
      builder.metadata()
        .title(spec.title)
        .artist(spec.artist)
        .album("Album")
        .year(spec.year)
        .trackNumber(spec.trackNumber);
      builder.property()
        .uri("/tmp/test.flac")
        .durationMs(spec.durationMs)
        .bitrate(320000)
        .sampleRate(44100)
        .channels(2)
        .bitDepth(16);

      if (!spec.customKey.empty())
      {
        builder.custom().add(spec.customKey, spec.customValue);
      }

      auto hotData = builder.serializeHot(txn, _library.dictionary());
      auto coldData = builder.serializeCold(txn, _library.dictionary(), _library.resources());
      auto [id, _] = writer.createHotCold(hotData, coldData);
      txn.commit();
      return id;
    }

    void updateTrack(TrackId id, std::move_only_function<void(TrackBuilder&)> mutate)
    {
      auto txn = _library.writeTransaction();
      auto writer = _library.tracks().writer(txn);
      auto view = writer.get(id, TrackStore::Reader::LoadMode::Both);
      REQUIRE(view.has_value());

      auto builder = TrackBuilder::fromView(*view, _library.dictionary());
      mutate(builder);

      auto hotData = builder.serializeHot(txn, _library.dictionary());
      auto coldData = builder.serializeCold(txn, _library.dictionary(), _library.resources());
      writer.updateHot(id, hotData);
      writer.updateCold(id, coldData);
      txn.commit();
    }

  private:
    TempDir _tempDir;
    MusicLibrary _library;
  };
} // namespace

TEST_CASE("SmartListEngine", "[app][smartlist]")
{
  SECTION("empty expression matches all tracks and maintains ID order")
  {
    auto testLibrary = TestMusicLibrary{};
    auto first = testLibrary.addTrack(makeTrackSpec("first", 2020));
    auto second = testLibrary.addTrack(makeTrackSpec("second", 2021));

    auto source = MutableTrackIdList{};
    // Source is [second, first]
    source.addInitial(second);
    source.addInitial(first);

    auto engine = SmartListEngine{testLibrary.library()};
    auto filtered = FilteredTrackIdList{source, testLibrary.library(), engine};
    auto spy = ObserverSpy{};
    filtered.attach(&spy);

    filtered.reload();

    REQUIRE_FALSE(filtered.hasError());
    REQUIRE(filtered.size() == 2);
    // flat_set maintains ID order: [first, second] since first(1) < second(2)
    CHECK(filtered.trackIdAt(0) == first);
    CHECK(filtered.trackIdAt(1) == second);
    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == ObserverSpy::EventKind::Reset);

    filtered.detach(&spy);
  }

  SECTION("reloading one list does not reset sibling lists sharing a source")
  {
    auto testLibrary = TestMusicLibrary{};
    auto first = testLibrary.addTrack(makeTrackSpec("first", 2020, 180000));
    auto second = testLibrary.addTrack(makeTrackSpec("second", 2022, 260000));

    auto source = MutableTrackIdList{};
    source.addInitial(first);
    source.addInitial(second);

    auto engine = SmartListEngine{testLibrary.library()};
    auto hotList = FilteredTrackIdList{source, testLibrary.library(), engine};
    auto coldList = FilteredTrackIdList{source, testLibrary.library(), engine};
    hotList.setExpression("$year >= 2021");
    hotList.reload();
    coldList.setExpression("@duration >= 240000");
    coldList.reload();

    auto hotSpy = ObserverSpy{};
    auto coldSpy = ObserverSpy{};
    hotList.attach(&hotSpy);
    coldList.attach(&coldSpy);

    hotList.setExpression("$year >= 2022");
    hotList.reload();

    REQUIRE(hotSpy.events.size() == 1);
    CHECK(hotSpy.events[0].kind == ObserverSpy::EventKind::Reset);
    CHECK(coldSpy.events.empty());
    REQUIRE(hotList.size() == 1);
    CHECK(hotList.trackIdAt(0) == second);
    REQUIRE(coldList.size() == 1);
    CHECK(coldList.trackIdAt(0) == second);

    hotList.detach(&hotSpy);
    coldList.detach(&coldSpy);
  }

  SECTION("source insert emits inserted notifications instead of bucket resets")
  {
    auto testLibrary = TestMusicLibrary{};
    auto original = testLibrary.addTrack(makeTrackSpec("old", 2020, 180000));

    auto source = MutableTrackIdList{};
    source.addInitial(original);

    auto engine = SmartListEngine{testLibrary.library()};
    auto hotList = FilteredTrackIdList{source, testLibrary.library(), engine};
    auto coldList = FilteredTrackIdList{source, testLibrary.library(), engine};
    hotList.setExpression("$year >= 2021");
    hotList.reload();
    coldList.setExpression("@duration >= 240000");
    coldList.reload();

    auto hotSpy = ObserverSpy{};
    auto coldSpy = ObserverSpy{};
    hotList.attach(&hotSpy);
    coldList.attach(&coldSpy);

    auto inserted = testLibrary.addTrack(makeTrackSpec("new", 2023, 250000));
    source.insert(inserted, 1);

    REQUIRE(hotSpy.events.size() == 1);
    CHECK(hotSpy.events[0].kind == ObserverSpy::EventKind::Inserted);
    CHECK(hotSpy.events[0].id == inserted);
    CHECK(hotSpy.events[0].index == 0);
    REQUIRE(coldSpy.events.size() == 1);
    CHECK(coldSpy.events[0].kind == ObserverSpy::EventKind::Inserted);
    CHECK(coldSpy.events[0].id == inserted);
    CHECK(coldSpy.events[0].index == 0);
    REQUIRE(hotList.size() == 1);
    CHECK(hotList.trackIdAt(0) == inserted);
    REQUIRE(coldList.size() == 1);
    CHECK(coldList.trackIdAt(0) == inserted);

    hotList.detach(&hotSpy);
    coldList.detach(&coldSpy);
  }

  SECTION("source update diffs membership transitions incrementally")
  {
    auto testLibrary = TestMusicLibrary{};
    auto trackId = testLibrary.addTrack(makeTrackSpec("track", 2020));

    auto source = MutableTrackIdList{};
    source.addInitial(trackId);

    auto engine = SmartListEngine{testLibrary.library()};
    auto filtered = FilteredTrackIdList{source, testLibrary.library(), engine};
    filtered.setExpression("$year >= 2021");
    filtered.reload();

    auto spy = ObserverSpy{};
    filtered.attach(&spy);

    testLibrary.updateTrack(trackId, [](TrackBuilder& builder) { builder.metadata().year(2022); });
    source.update(trackId);

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == ObserverSpy::EventKind::Inserted);
    CHECK(spy.events[0].id == trackId);
    CHECK(spy.events[0].index == 0);

    spy.clear();

    testLibrary.updateTrack(trackId, [](TrackBuilder& builder) { builder.metadata().title("renamed"); });
    source.update(trackId);

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == ObserverSpy::EventKind::Updated);
    CHECK(spy.events[0].id == trackId);
    CHECK(spy.events[0].index == 0);

    spy.clear();

    testLibrary.updateTrack(trackId, [](TrackBuilder& builder) { builder.metadata().year(2019); });
    source.update(trackId);

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == ObserverSpy::EventKind::Removed);
    CHECK(spy.events[0].id == trackId);
    CHECK(spy.events[0].index == 0);
    CHECK(filtered.size() == 0);

    filtered.detach(&spy);
  }

  SECTION("child filtered lists track parent membership as their source")
  {
    auto testLibrary = TestMusicLibrary{};
    auto legacy = testLibrary.addTrack(makeTrackSpec("legacy", 2020, 180000));
    auto modern = testLibrary.addTrack(makeTrackSpec("modern", 2022, 250000));

    auto source = MutableTrackIdList{};
    source.addInitial(legacy);
    source.addInitial(modern);

    auto engine = SmartListEngine{testLibrary.library()};
    auto parent = FilteredTrackIdList{source, testLibrary.library(), engine};
    parent.setExpression("$year >= 2021");
    parent.reload();

    auto child = FilteredTrackIdList{parent, testLibrary.library(), engine};
    child.setExpression("@duration >= 240000");
    child.reload();

    REQUIRE(parent.size() == 1);
    CHECK(parent.trackIdAt(0) == modern);
    REQUIRE(child.size() == 1);
    CHECK(child.trackIdAt(0) == modern);

    auto spy = ObserverSpy{};
    child.attach(&spy);

    auto fresh = testLibrary.addTrack(makeTrackSpec("fresh", 2023, 260000));
    source.insert(fresh, 2);

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == ObserverSpy::EventKind::Inserted);
    CHECK(spy.events[0].id == fresh);
    CHECK(spy.events[0].index == 1);
    REQUIRE(child.size() == 2);
    CHECK(child.trackIdAt(1) == fresh);

    spy.clear();

    testLibrary.updateTrack(modern, [](TrackBuilder& builder) { builder.metadata().year(2019); });
    source.update(modern);

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == ObserverSpy::EventKind::Removed);
    CHECK(spy.events[0].id == modern);
    CHECK(spy.events[0].index == 0);
    REQUIRE(child.size() == 1);
    CHECK(child.trackIdAt(0) == fresh);

    child.detach(&spy);
  }

  SECTION("invalid expression is isolated from sibling lists")
  {
    auto testLibrary = TestMusicLibrary{};
    auto first = testLibrary.addTrack(makeTrackSpec("first", 2022));

    auto source = MutableTrackIdList{};
    source.addInitial(first);

    auto engine = SmartListEngine{testLibrary.library()};
    auto validList = FilteredTrackIdList{source, testLibrary.library(), engine};
    auto invalidList = FilteredTrackIdList{source, testLibrary.library(), engine};
    validList.setExpression("$year >= 2021");
    validList.reload();
    invalidList.setExpression("   ");
    invalidList.reload();

    REQUIRE_FALSE(validList.hasError());
    REQUIRE(validList.size() == 1);
    REQUIRE(invalidList.hasError());
    CHECK_FALSE(invalidList.errorMessage().empty());
    CHECK(invalidList.size() == 0);

    auto validSpy = ObserverSpy{};
    auto invalidSpy = ObserverSpy{};
    validList.attach(&validSpy);
    invalidList.attach(&invalidSpy);

    auto second = testLibrary.addTrack(makeTrackSpec("second", 2023));
    source.insert(second, 1);

    REQUIRE(validSpy.events.size() == 1);
    CHECK(validSpy.events[0].kind == ObserverSpy::EventKind::Inserted);
    CHECK(validSpy.events[0].id == second);
    CHECK(invalidSpy.events.empty());
    REQUIRE(validList.size() == 2);
    CHECK(validList.trackIdAt(1) == second);
    CHECK(invalidList.size() == 0);

    validList.detach(&validSpy);
    invalidList.detach(&invalidSpy);
  }

  SECTION("source destruction is handled gracefully")
  {
    auto testLibrary = TestMusicLibrary{};
    auto engine = SmartListEngine{testLibrary.library()};

    auto* source = new MutableTrackIdList();
    auto filtered = std::make_unique<FilteredTrackIdList>(*source, testLibrary.library(), engine);

    auto spy = ObserverSpy{};
    filtered->attach(&spy);

    // Destroy source while filtered list is still alive
    delete source;

    // Filtered list should be notified of reset (source is gone)
    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == ObserverSpy::EventKind::Reset);

    // Now destroy filtered list - this calls unregisterList
    // This should not crash despite source being gone!
    filtered.reset();
  }

  SECTION("batch operations emit batch notifications")
  {
    auto testLibrary = TestMusicLibrary{};
    auto engine = SmartListEngine{testLibrary.library()};
    auto source = MutableTrackIdList{};

    auto list = FilteredTrackIdList{source, testLibrary.library(), engine};
    list.setExpression("$year >= 2020");
    list.reload();

    auto spy = ObserverSpy{};
    list.attach(&spy);

    auto t1 = testLibrary.addTrack(makeTrackSpec("Old", 2010));
    auto t2 = testLibrary.addTrack(makeTrackSpec("New1", 2021));
    auto t3 = testLibrary.addTrack(makeTrackSpec("New2", 2022));

    TrackId batchArray[] = {t1, t2, t3};
    source.batchInsert(batchArray);

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == ObserverSpy::EventKind::BatchInserted);
    // t1 should be filtered out
    REQUIRE(spy.events[0].batchIds.size() == 2);
    CHECK(std::ranges::contains(spy.events[0].batchIds, t2));
    CHECK(std::ranges::contains(spy.events[0].batchIds, t3));
    CHECK(list.size() == 2);

    spy.clear();
    TrackId removeArray[] = {t2};
    source.batchRemove(removeArray);

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == ObserverSpy::EventKind::BatchRemoved);
    REQUIRE(spy.events[0].batchIds.size() == 1);
    CHECK(spy.events[0].batchIds[0] == t2);
    CHECK(list.size() == 1);

    list.detach(&spy);
  }

  SECTION("load mode optimization for mixed access profiles")
  {
    auto testLibrary = TestMusicLibrary{};
    auto engine = SmartListEngine{testLibrary.library()};
    auto source = MutableTrackIdList{};

    auto hotList = FilteredTrackIdList{source, testLibrary.library(), engine};
    hotList.setExpression("$year >= 2020"); // Hot metadata

    auto coldList = FilteredTrackIdList{source, testLibrary.library(), engine};
    coldList.setExpression("@duration >= 180000"); // Cold property

    auto t1 = testLibrary.addTrack(makeTrackSpec("Track", 2022, 200000));
    TrackId batchArray[] = {t1};
    source.batchInsert(batchArray);

    hotList.reload();
    coldList.reload();

    CHECK(hotList.size() == 1);
    CHECK(coldList.size() == 1);
  }
}
