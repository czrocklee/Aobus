// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/runtime/TrackSourceTestSupport.h"
#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSource.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    using namespace ao::library;

    struct TrackSpec final
    {
      std::string title = "Track";
      std::string artist = "Artist";
      std::uint16_t year = 0;
      std::uint16_t trackNumber = 0;
      std::chrono::milliseconds duration = std::chrono::seconds{180};
      std::string customKey;
      std::string customValue;
    };

    TrackSpec makeTrackSpec(std::string_view title,
                            std::uint16_t year,
                            std::chrono::milliseconds duration = std::chrono::seconds{180})
    {
      auto spec = TrackSpec{};
      spec.title = title;
      spec.year = year;
      spec.duration = duration;
      return spec;
    }

    struct ObserverSpy final : public TrackSourceObserver
    {
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

      void onBulkInserted(std::span<TrackId const> ids) override
      {
        events.push_back({.kind = EventKind::BatchInserted, .batchIds = {ids.begin(), ids.end()}});
      }

      void onBulkUpdated(std::span<TrackId const> ids) override
      {
        events.push_back({.kind = EventKind::BatchUpdated, .batchIds = {ids.begin(), ids.end()}});
      }

      void onBulkRemoved(std::span<TrackId const> ids) override
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
        : _tempDir{}, _library{_tempDir.path(), _tempDir.path()}
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
          .duration(spec.duration)
          .bitrate(Bitrate{320000})
          .sampleRate(SampleRate{44100})
          .channels(Channels{2})
          .bitDepth(BitDepth{16});

        if (!spec.customKey.empty())
        {
          builder.customMetadata().add(spec.customKey, spec.customValue);
        }

        auto hotData = builder.serializeHot(txn, _library.dictionary());
        REQUIRE(hotData);
        auto coldData = builder.serializeCold(txn, _library.dictionary(), _library.resources());
        REQUIRE(coldData);
        auto [id, _] = ao::test::requireValue(writer.createHotCold(*hotData, *coldData));
        REQUIRE(txn.commit());
        return id;
      }

      void updateTrack(TrackId id, std::move_only_function<void(TrackBuilder&)> mutate)
      {
        auto txn = _library.writeTransaction();
        auto writer = _library.tracks().writer(txn);
        auto optView = writer.get(id, TrackStore::Reader::LoadMode::Both);
        REQUIRE(optView.has_value());

        auto builder = TrackBuilder::fromView(*optView, _library.dictionary());
        mutate(builder);

        auto hotData = builder.serializeHot(txn, _library.dictionary());
        REQUIRE(hotData);
        auto coldData = builder.serializeCold(txn, _library.dictionary(), _library.resources());
        REQUIRE(coldData);
        REQUIRE(writer.updateHot(id, *hotData));
        REQUIRE(writer.updateCold(
          id, coldData->size(), [&](std::span<std::byte> buf) { std::ranges::copy(*coldData, buf.begin()); }));
        REQUIRE(txn.commit());
      }

    private:
      ao::test::TempDir _tempDir;
      MusicLibrary _library;
    };
  } // namespace

  TEST_CASE("rt::SmartListEvaluator", "[app][unit][smartlist]")
  {
    SECTION("empty expression matches all tracks and maintains ID order")
    {
      auto testLibrary = TestMusicLibrary{};
      auto first = testLibrary.addTrack(makeTrackSpec("first", 2020));
      auto second = testLibrary.addTrack(makeTrackSpec("second", 2021));

      auto source = MutableTrackSource{};
      // Source is [second, first]
      source.addInitial(second);
      source.addInitial(first);

      auto engine = SmartListEvaluator{testLibrary.library()};
      auto filtered = SmartListSource{source, testLibrary.library(), engine};
      auto spy = ObserverSpy{};
      filtered.attach(&spy);

      filtered.reload();

      CHECK_FALSE(filtered.hasError());
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
      auto first = testLibrary.addTrack(makeTrackSpec("first", 2020, std::chrono::seconds{180}));
      auto second = testLibrary.addTrack(makeTrackSpec("second", 2022, std::chrono::seconds{260}));

      auto source = MutableTrackSource{};
      source.addInitial(first);
      source.addInitial(second);

      auto engine = SmartListEvaluator{testLibrary.library()};
      auto hotList = SmartListSource{source, testLibrary.library(), engine};
      auto coldList = SmartListSource{source, testLibrary.library(), engine};
      hotList.setExpression("$year >= 2021");
      hotList.reload();
      coldList.setExpression("@duration >= 4m");
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
      auto original = testLibrary.addTrack(makeTrackSpec("old", 2020, std::chrono::seconds{180}));

      auto source = MutableTrackSource{};
      source.addInitial(original);

      auto engine = SmartListEvaluator{testLibrary.library()};
      auto hotList = SmartListSource{source, testLibrary.library(), engine};
      auto coldList = SmartListSource{source, testLibrary.library(), engine};
      hotList.setExpression("$year >= 2021");
      hotList.reload();
      coldList.setExpression("@duration >= 4m");
      coldList.reload();

      auto hotSpy = ObserverSpy{};
      auto coldSpy = ObserverSpy{};
      hotList.attach(&hotSpy);
      coldList.attach(&coldSpy);

      auto inserted = testLibrary.addTrack(makeTrackSpec("new", 2023, std::chrono::seconds{250}));
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

      auto source = MutableTrackSource{};
      source.addInitial(trackId);

      auto engine = SmartListEvaluator{testLibrary.library()};
      auto filtered = SmartListSource{source, testLibrary.library(), engine};
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
      auto legacy = testLibrary.addTrack(makeTrackSpec("legacy", 2020, std::chrono::seconds{180}));
      auto modern = testLibrary.addTrack(makeTrackSpec("modern", 2022, std::chrono::seconds{250}));

      auto source = MutableTrackSource{};
      source.addInitial(legacy);
      source.addInitial(modern);

      auto engine = SmartListEvaluator{testLibrary.library()};
      auto parent = SmartListSource{source, testLibrary.library(), engine};
      parent.setExpression("$year >= 2021");
      parent.reload();

      auto child = SmartListSource{parent, testLibrary.library(), engine};
      child.setExpression("@duration >= 4m");
      child.reload();

      REQUIRE(parent.size() == 1);
      CHECK(parent.trackIdAt(0) == modern);
      REQUIRE(child.size() == 1);
      CHECK(child.trackIdAt(0) == modern);

      auto spy = ObserverSpy{};
      child.attach(&spy);

      auto fresh = testLibrary.addTrack(makeTrackSpec("fresh", 2023, std::chrono::seconds{260}));
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

      auto source = MutableTrackSource{};
      source.addInitial(first);

      auto engine = SmartListEvaluator{testLibrary.library()};
      auto validList = SmartListSource{source, testLibrary.library(), engine};
      auto invalidList = SmartListSource{source, testLibrary.library(), engine};
      validList.setExpression("$year >= 2021");
      validList.reload();
      invalidList.setExpression("   ");
      invalidList.reload();

      CHECK_FALSE(validList.hasError());
      CHECK(validList.size() == 1);
      CHECK(invalidList.hasError());
      REQUIRE(invalidList.error().has_value());
      CHECK(invalidList.error()->code == Error::Code::FormatRejected);
      CHECK_FALSE(invalidList.error()->message.empty());
      CHECK(invalidList.size() == 0);

      SECTION("Standard flow: invalid expression remains empty")
      {
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

      SECTION("State recovery: Setting a valid expression clears error")
      {
        invalidList.setExpression("$year > 2000");
        invalidList.reload();

        CHECK_FALSE(invalidList.hasError());
        CHECK_FALSE(invalidList.error().has_value());
        CHECK(invalidList.size() == 1); // Only 'first' is in source at this point
      }
    }

    SECTION("source destruction is handled gracefully")
    {
      auto testLibrary = TestMusicLibrary{};
      auto engine = SmartListEvaluator{testLibrary.library()};

      auto sourcePtr = std::make_unique<MutableTrackSource>();
      auto filteredPtr = std::make_unique<SmartListSource>(*sourcePtr, testLibrary.library(), engine);

      auto spy = ObserverSpy{};
      filteredPtr->attach(&spy);

      // Destroy source while filtered list is still alive
      sourcePtr = nullptr;

      // Filtered list should be notified of reset (source is gone)
      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::Reset);

      // Now destroy filtered list - this calls unregisterList
      // This should not crash despite source being gone!
      filteredPtr.reset();
    }

    SECTION("batch operations emit batch notifications")
    {
      auto testLibrary = TestMusicLibrary{};
      auto engine = SmartListEvaluator{testLibrary.library()};
      auto source = MutableTrackSource{};

      auto list = SmartListSource{source, testLibrary.library(), engine};
      list.setExpression("$year >= 2020");
      list.reload();

      auto spy = ObserverSpy{};
      list.attach(&spy);

      auto t1 = testLibrary.addTrack(makeTrackSpec("Old", 2010));
      auto t2 = testLibrary.addTrack(makeTrackSpec("New1", 2021));
      auto t3 = testLibrary.addTrack(makeTrackSpec("New2", 2022));

      auto const batchArray = std::to_array<TrackId>({t1, t2, t3});
      source.batchInsert(batchArray);

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::BatchInserted);
      // t1 should be filtered out
      CHECK(spy.events[0].batchIds.size() == 2);
      CHECK(std::ranges::contains(spy.events[0].batchIds, t2));
      CHECK(std::ranges::contains(spy.events[0].batchIds, t3));
      CHECK(list.size() == 2);

      spy.clear();
      auto const removeArray = std::to_array<TrackId>({t2});
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
      auto engine = SmartListEvaluator{testLibrary.library()};
      auto source = MutableTrackSource{};

      auto hotList = SmartListSource{source, testLibrary.library(), engine};
      hotList.setExpression("$year >= 2020"); // Hot metadata

      auto coldList = SmartListSource{source, testLibrary.library(), engine};
      coldList.setExpression("@duration >= 3m"); // Cold property

      auto t1 = testLibrary.addTrack(makeTrackSpec("Track", 2022, std::chrono::seconds{200}));
      auto const batchArray = std::array{t1};
      source.batchInsert(batchArray);

      hotList.reload();
      coldList.reload();

      CHECK(hotList.size() == 1);
      CHECK(coldList.size() == 1);
    }

    SECTION("evaluator destruction detaches from sources")
    {
      auto testLibrary = TestMusicLibrary{};
      auto source = MutableTrackSource{};
      auto enginePtr = std::make_unique<SmartListEvaluator>(testLibrary.library());
      auto listPtr = std::make_unique<SmartListSource>(source, testLibrary.library(), *enginePtr);

      // Destroy engine BEFORE source and list
      // Note: list won't be able to reload anymore but we just want to hit the destructor
      enginePtr.reset();
      listPtr.reset();
    }

    SECTION("single mutations on base source are handled correctly")
    {
      auto testLibrary = TestMusicLibrary{};
      auto engine = SmartListEvaluator{testLibrary.library()};
      auto source = MutableTrackSource{};

      auto list = SmartListSource{source, testLibrary.library(), engine};
      list.setExpression("$year >= 2020");
      list.reload();

      auto t1 = testLibrary.addTrack(makeTrackSpec("Old", 2010));
      auto t2 = testLibrary.addTrack(makeTrackSpec("New1", 2021));

      // Single Insert
      source.insert(t2, source.size());
      CHECK(list.size() == 1);

      source.insert(t1, source.size());
      CHECK(list.size() == 1);

      // Single Update
      testLibrary.updateTrack(t1, [](TrackBuilder& builder) { builder.metadata().year(2022); });
      source.update(t1);
      CHECK(list.size() == 2);

      // Single Remove
      source.remove(t2);
      CHECK(list.size() == 1);
    }

    SECTION("batch mutations trigger flat_set optimizations")
    {
      auto testLibrary = TestMusicLibrary{};
      auto engine = SmartListEvaluator{testLibrary.library()};
      auto source = MutableTrackSource{};

      auto list = SmartListSource{source, testLibrary.library(), engine};
      list.setExpression("$year >= 2020");
      list.reload();

      auto t1 = testLibrary.addTrack(makeTrackSpec("Old", 2010));
      auto t2 = testLibrary.addTrack(makeTrackSpec("New1", 2021));
      auto t3 = testLibrary.addTrack(makeTrackSpec("New2", 2022));

      auto const batchArray = std::to_array<TrackId>({t1, t2, t3});
      source.batchInsert(batchArray);
      CHECK(list.size() == 2);

      auto const removeArray = std::to_array<TrackId>({t1});
      source.batchRemove(removeArray);
      CHECK(list.size() == 2); // t2 and t3

      // Trigger batch update that causes transitions
      // t2 matches -> won't match (removed)
      // t1 doesn't match -> will match (inserted)
      // t3 matches -> still matches (updated)
      testLibrary.updateTrack(t2, [](TrackBuilder& builder) { builder.metadata().year(2010); }); // Remove
      source.insert(t1, source.size()); // re-add t1 to source so it can be updated
      testLibrary.updateTrack(t1, [](TrackBuilder& builder) { builder.metadata().year(2025); });       // Insert
      testLibrary.updateTrack(t3, [](TrackBuilder& builder) { builder.metadata().title("Updated"); }); // Update

      auto const updateArray = std::to_array<TrackId>({t1, t2, t3});
      source.batchUpdate(updateArray);

      CHECK(list.size() == 2); // t1 and t3
    }

    SECTION("HotAndCold access profile optimization")
    {
      auto testLibrary = TestMusicLibrary{};
      auto engine = SmartListEvaluator{testLibrary.library()};
      auto source = MutableTrackSource{};

      auto list = SmartListSource{source, testLibrary.library(), engine};
      // Requires both metadata and property reader
      list.setExpression("$year >= 2020 && @duration >= 3m");

      auto t1 = testLibrary.addTrack(makeTrackSpec("Track", 2022, std::chrono::seconds{200}));
      auto t2 = testLibrary.addTrack(makeTrackSpec("Bad", 2022, std::chrono::seconds{1}));
      auto const batchArray = std::array{t1, t2};
      source.batchInsert(batchArray);

      list.reload();
      CHECK(list.size() == 1);
      CHECK(list.trackIdAt(0) == t1);
    }

    SECTION("SmartListSource::notifyUpdated relays to evaluator")
    {
      auto testLibrary = TestMusicLibrary{};
      auto engine = SmartListEvaluator{testLibrary.library()};
      auto source = MutableTrackSource{};

      auto list = SmartListSource{source, testLibrary.library(), engine};
      list.setExpression("$year >= 2020");
      list.reload();

      auto t1 = testLibrary.addTrack(makeTrackSpec("Track", 2020));
      auto const batchArray = std::array{t1};
      source.batchInsert(batchArray);

      // Mutate via base library, then notify list directly
      testLibrary.updateTrack(t1, [](TrackBuilder& builder) { builder.metadata().year(2022); });
      list.notifyUpdated(t1);

      CHECK(list.size() == 1);
    }

    SECTION("SmartListSource::indexOf and TrackSource::notifyUpdated coverage")
    {
      auto testLibrary = TestMusicLibrary{};
      auto engine = SmartListEvaluator{testLibrary.library()};
      auto source = MutableTrackSource{};

      auto list = SmartListSource{source, testLibrary.library(), engine};
      list.setExpression("$year >= 2020");
      list.reload();

      auto t1 = testLibrary.addTrack(makeTrackSpec("Track1", 2020));
      auto t2 = testLibrary.addTrack(makeTrackSpec("Track2", 2010));
      auto t3 = testLibrary.addTrack(makeTrackSpec("Track3", 2021));

      source.batchInsert(std::to_array<TrackId>({t1, t2, t3}));

      // Test SmartListSource::indexOf
      CHECK(list.indexOf(t1) == 0);                      // Present
      CHECK(list.indexOf(t3) == 1);                      // Present (flat_set sorted by ID)
      CHECK(list.indexOf(t2) == std::nullopt);           // Filtered out
      CHECK(list.indexOf(TrackId{999}) == std::nullopt); // Non-existent

      // Test TrackSource::notifyUpdated(TrackId id) without index
      auto spy = ObserverSpy{};
      source.attach(&spy);

      // Call the base class method directly
      source.TrackSource::notifyUpdated(t3); // Calls TrackSource::notifyUpdated(id)

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::Updated);
      CHECK(spy.events[0].id == t3);

      // t2 is not in source's indexOf (wait, it IS in source, just not in list)
      // Let's call it on a non-existent track
      spy.clear();
      source.TrackSource::notifyUpdated(TrackId{999});
      CHECK(spy.events.empty()); // Should not notify since it's not in the source

      source.detach(&spy);

      // Destructor for TrackSource is implicitly covered when MutableTrackSource is destroyed
    }
  }
} // namespace ao::rt::test
