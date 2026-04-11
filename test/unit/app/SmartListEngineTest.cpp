// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <app/model/FilteredTrackIdList.h>
#include <app/model/SmartListEngine.h>
#include <app/model/TrackIdList.h>
#include <rs/core/MusicLibrary.h>
#include <rs/core/TrackBuilder.h>
#include <rs/core/TrackStore.h>
#include <test/unit/lmdb/TestUtils.h>

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

  using app::model::FilteredTrackIdList;
  using app::model::SmartListEngine;
  using app::model::TrackIdList;
  using app::model::TrackIdListObserver;
  using rs::core::MusicLibrary;
  using rs::core::TrackBuilder;
  using rs::core::TrackId;
  using rs::core::TrackStore;

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

  class MutableTrackIdList final : public TrackIdList
  {
  public:
    void addInitial(TrackId id)
    {
      _ids.push_back(id);
    }

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

    std::size_t size() const override { return _ids.size(); }

    TrackId trackIdAt(std::size_t index) const override
    {
      return _ids.at(index);
    }

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
    };

    struct Event
    {
      EventKind kind;
      TrackId id{};
      std::size_t index = 0;
    };

    void onReset() override
    {
      events.push_back({.kind = EventKind::Reset});
    }

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

    void clear()
    {
      events.clear();
    }

    std::vector<Event> events;
  };

  class TestMusicLibrary final
  {
  public:
    TestMusicLibrary()
      : _tempDir{}
      , _library{_tempDir.path()}
    {
    }

    MusicLibrary& library() { return _library; }

    TrackId addTrack(TrackSpec const& spec)
    {
      auto txn = _library.writeTransaction();
      auto writer = _library.tracks().writer(txn);

      auto builder = TrackBuilder::createNew();
      builder.metadata().title(spec.title).artist(spec.artist).album("Album").year(spec.year).trackNumber(spec.trackNumber);
      builder.property().uri("/tmp/test.flac").durationMs(spec.durationMs).bitrate(320000).sampleRate(44100).channels(2).bitDepth(16);

      if (!spec.customKey.empty())
      {
        builder.custom().add(spec.customKey, spec.customValue);
      }

      auto hotData = builder.serializeHot(txn, _library.dictionary());
      auto coldData = builder.serializeCold(txn, _library.dictionary(), _library.resources());
      auto [id, _view] = writer.createHotCold(hotData, coldData);
      (void)_view;
      txn.commit();
      return id;
    }

    void updateTrack(TrackId id, std::function<void(TrackBuilder&)> const& mutate)
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
  SECTION("empty expression matches all tracks and preserves source order")
  {
    TestMusicLibrary testLibrary;
    auto first = testLibrary.addTrack({.title = "first", .year = 2020});
    auto second = testLibrary.addTrack({.title = "second", .year = 2021});

    MutableTrackIdList source;
    source.addInitial(second);
    source.addInitial(first);

    SmartListEngine engine(testLibrary.library());
    FilteredTrackIdList filtered(source, testLibrary.library(), engine);
    ObserverSpy spy;
    filtered.attach(&spy);

    filtered.reload();

    REQUIRE_FALSE(filtered.hasError());
    REQUIRE(filtered.size() == 2);
    CHECK(filtered.trackIdAt(0) == second);
    CHECK(filtered.trackIdAt(1) == first);
    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == ObserverSpy::EventKind::Reset);

    filtered.detach(&spy);
  }

  SECTION("reloading one list does not reset sibling lists sharing a source")
  {
    TestMusicLibrary testLibrary;
    auto first = testLibrary.addTrack({.title = "first", .year = 2020, .durationMs = 180000});
    auto second = testLibrary.addTrack({.title = "second", .year = 2022, .durationMs = 260000});

    MutableTrackIdList source;
    source.addInitial(first);
    source.addInitial(second);

    SmartListEngine engine(testLibrary.library());
    FilteredTrackIdList hotList(source, testLibrary.library(), engine);
    FilteredTrackIdList coldList(source, testLibrary.library(), engine);
    hotList.setExpression("$year >= 2021");
    hotList.reload();
    coldList.setExpression("@duration >= 240000");
    coldList.reload();

    ObserverSpy hotSpy;
    ObserverSpy coldSpy;
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
    TestMusicLibrary testLibrary;
    auto original = testLibrary.addTrack({.title = "old", .year = 2020, .durationMs = 180000});

    MutableTrackIdList source;
    source.addInitial(original);

    SmartListEngine engine(testLibrary.library());
    FilteredTrackIdList hotList(source, testLibrary.library(), engine);
    FilteredTrackIdList coldList(source, testLibrary.library(), engine);
    hotList.setExpression("$year >= 2021");
    hotList.reload();
    coldList.setExpression("@duration >= 240000");
    coldList.reload();

    ObserverSpy hotSpy;
    ObserverSpy coldSpy;
    hotList.attach(&hotSpy);
    coldList.attach(&coldSpy);

    auto inserted = testLibrary.addTrack({.title = "new", .year = 2023, .durationMs = 250000});
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
    TestMusicLibrary testLibrary;
    auto trackId = testLibrary.addTrack({.title = "track", .year = 2020});

    MutableTrackIdList source;
    source.addInitial(trackId);

    SmartListEngine engine(testLibrary.library());
    FilteredTrackIdList filtered(source, testLibrary.library(), engine);
    filtered.setExpression("$year >= 2021");
    filtered.reload();

    ObserverSpy spy;
    filtered.attach(&spy);

    testLibrary.updateTrack(trackId, [](TrackBuilder& builder)
    {
      builder.metadata().year(2022);
    });
    source.update(trackId);

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == ObserverSpy::EventKind::Inserted);
    CHECK(spy.events[0].id == trackId);
    CHECK(spy.events[0].index == 0);

    spy.clear();

    testLibrary.updateTrack(trackId, [](TrackBuilder& builder)
    {
      builder.metadata().title("renamed");
    });
    source.update(trackId);

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == ObserverSpy::EventKind::Updated);
    CHECK(spy.events[0].id == trackId);
    CHECK(spy.events[0].index == 0);

    spy.clear();

    testLibrary.updateTrack(trackId, [](TrackBuilder& builder)
    {
      builder.metadata().year(2019);
    });
    source.update(trackId);

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == ObserverSpy::EventKind::Removed);
    CHECK(spy.events[0].id == trackId);
    CHECK(spy.events[0].index == 0);
    CHECK(filtered.size() == 0);

    filtered.detach(&spy);
  }

  SECTION("invalid expression is isolated from sibling lists")
  {
    TestMusicLibrary testLibrary;
    auto first = testLibrary.addTrack({.title = "first", .year = 2022});

    MutableTrackIdList source;
    source.addInitial(first);

    SmartListEngine engine(testLibrary.library());
    FilteredTrackIdList validList(source, testLibrary.library(), engine);
    FilteredTrackIdList invalidList(source, testLibrary.library(), engine);
    validList.setExpression("$year >= 2021");
    validList.reload();
    invalidList.setExpression("$year =");
    invalidList.reload();

    REQUIRE_FALSE(validList.hasError());
    REQUIRE(validList.size() == 1);
    REQUIRE(invalidList.hasError());
    CHECK_FALSE(invalidList.errorMessage().empty());
    CHECK(invalidList.size() == 0);

    ObserverSpy validSpy;
    ObserverSpy invalidSpy;
    validList.attach(&validSpy);
    invalidList.attach(&invalidSpy);

    auto second = testLibrary.addTrack({.title = "second", .year = 2023});
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
}
