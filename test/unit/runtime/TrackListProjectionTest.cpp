// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TestUtils.h"
#include "ao/Type.h"
#include "ao/library/MusicLibrary.h"
#include "ao/library/TrackStore.h"
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/ProjectionTypes.h>
#include <ao/rt/SmartListEvaluator.h>
#include <ao/rt/SmartListSource.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackListProjection.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/TrackSource.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    using namespace ao::library;
    using namespace ao::lmdb::test;

    class MutableTrackSource final : public TrackSource
    {
    public:
      void addInitial(TrackId id) { _ids.push_back(id); }
      void batchInsert(std::span<TrackId const> ids)
      {
        _ids.insert(_ids.end(), ids.begin(), ids.end());
        notifyInserted(ids);
      }

      void batchRemove(std::span<TrackId const> ids)
      {
        for (auto id : ids)
        {
          std::erase(_ids, id);
        }

        notifyRemoved(ids);
      }

      void batchUpdate(std::span<TrackId const> ids) { notifyUpdated(ids); }

      void singleInsert(TrackId id)
      {
        _ids.push_back(id);
        notifyInserted(id, _ids.size() - 1);
      }

      void singleRemove(TrackId id)
      {
        if (auto it = std::ranges::find(_ids, id); it != _ids.end())
        {
          auto index = std::distance(_ids.begin(), it);
          _ids.erase(it);
          notifyRemoved(id, static_cast<std::size_t>(index));
        }
      }

      void singleUpdate(TrackId id)
      {
        if (auto it = std::ranges::find(_ids, id); it != _ids.end())
        {
          auto index = std::distance(_ids.begin(), it);
          notifyUpdated(id, static_cast<std::size_t>(index));
        }
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

    struct TestEnv final
    {
      TestMusicLibrary lib;
      MutableTrackSource source;
      SmartListEvaluator engine;
      std::unique_ptr<SmartListSource> filtered;

      TestEnv()
        : engine{lib.library()}
      {
      }

      TrackListProjection createProjection(ViewId viewId)
      {
        return TrackListProjection{viewId, *filtered, lib.library()};
      }

      void setupFiltered(std::span<TrackId const> ids)
      {
        for (auto id : ids)
        {
          source.addInitial(id);
        }

        filtered = std::make_unique<SmartListSource>(source, lib.library(), engine);
        filtered->reload();
      }
    };
  }

  TEST_CASE("TrackListProjection - basic lifecycle with data", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};
    auto const id1 = env.lib.addTrack(makeSpec("Track A", 2020));
    auto const id2 = env.lib.addTrack(makeSpec("Track B", 2021));
    env.setupFiltered({{id1, id2}});

    auto proj = env.createProjection(ViewId{1});

    SECTION("size matches source")
    {
      CHECK(proj.size() == 2);
    }

    SECTION("trackIdAt returns correct tracks in ID order")
    {
      CHECK(proj.trackIdAt(0) == id1);
      CHECK(proj.trackIdAt(1) == id2);
    }

    SECTION("viewId and revision")
    {
      CHECK(proj.viewId() == ViewId{1});
      CHECK(proj.revision() == 0);
    }

    SECTION("trackIdAt out of bounds returns invalid")
    {
      CHECK(proj.trackIdAt(999) == kInvalidTrackId);
    }

    SECTION("indexOf returns correct positions")
    {
      CHECK(proj.indexOf(id1) == 0);
      CHECK(proj.indexOf(id2) == 1);
      CHECK_FALSE(proj.indexOf(TrackId{999}).has_value());
    }
  }

  TEST_CASE("TrackListProjection - normalizeTitle behavior", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};
    auto const id1 = env.lib.addTrack(TrackSpec{.title = "The Best"});
    auto const id2 = env.lib.addTrack(TrackSpec{.title = "A Better"});
    auto const id3 = env.lib.addTrack(TrackSpec{.title = "An Apple"});
    auto const id4 = env.lib.addTrack(TrackSpec{.title = "Zeppelin"});
    auto const id5 = env.lib.addTrack(TrackSpec{.title = "the other"});
    auto const id6 = env.lib.addTrack(TrackSpec{.title = "a different"});
    auto const id7 = env.lib.addTrack(TrackSpec{.title = "ANOTHER"});

    env.setupFiltered({{id1, id2, id3, id4, id5, id6, id7}});

    auto proj = env.createProjection(ViewId{1});
    auto const sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{
      .groupBy = TrackGroupKey::None, .sortBy = {TrackSortTerm{.field = TrackSortField::Title, .ascending = true}}});

    // Expected order (normalized):
    // ANOTHER -> another
    // An Apple -> apple
    // The Best -> best
    // A Better -> better
    // a different -> different
    // the other -> other
    // Zeppelin -> zeppelin

    REQUIRE(proj.size() == 7);
    CHECK(proj.trackIdAt(0) == id7); // ANOTHER
    CHECK(proj.trackIdAt(1) == id3); // An Apple
    CHECK(proj.trackIdAt(2) == id1); // The Best
    CHECK(proj.trackIdAt(3) == id2); // A Better
    CHECK(proj.trackIdAt(4) == id6); // a different
    CHECK(proj.trackIdAt(5) == id5); // the other
    CHECK(proj.trackIdAt(6) == id4); // Zeppelin
  }

  TEST_CASE("TrackListProjection - subscribe reset-on-subscribe", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};
    auto const id1 = env.lib.addTrack(makeSpec("Track", 2020));
    env.setupFiltered({{id1}});

    auto proj = env.createProjection(ViewId{1});
    bool receivedReset = false;
    auto const sub = proj.subscribe(
      [&](TrackListProjectionDeltaBatch const& batch)
      {
        REQUIRE(batch.deltas.size() == 1);
        CHECK(std::holds_alternative<ProjectionReset>(batch.deltas[0]));
        receivedReset = true;
      });

    CHECK(receivedReset);
  }

  TEST_CASE("TrackListProjection - multiple subscribers", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};
    auto const id1 = env.lib.addTrack(makeSpec("Track", 2020));
    env.setupFiltered({{id1}});

    auto proj = env.createProjection(ViewId{1});
    std::int32_t count = 0;
    auto const sub1 = proj.subscribe([&](TrackListProjectionDeltaBatch const&) { ++count; });
    auto const sub2 = proj.subscribe([&](TrackListProjectionDeltaBatch const&) { ++count; });
    CHECK(count == 2);
  }

  TEST_CASE("TrackListProjection - unsubscribed handler not called", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};
    auto const id1 = env.lib.addTrack(makeSpec("Track", 2020));
    env.setupFiltered({{id1}});

    auto proj = env.createProjection(ViewId{1});
    std::int32_t count = 0;
    auto sub = proj.subscribe([&](TrackListProjectionDeltaBatch const&) { ++count; });
    auto const initial = count;
    sub.reset();
    CHECK(count == initial);
  }

  TEST_CASE("TrackListProjection - empty projection", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};
    env.setupFiltered({});

    auto proj = env.createProjection(ViewId{1});
    CHECK(proj.size() == 0);
    CHECK(proj.trackIdAt(0) == kInvalidTrackId);
    CHECK(proj.trackIdAt(999) == kInvalidTrackId);
  }

  TEST_CASE("TrackListProjection - sort 20 tracks by year then title", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};
    auto ids = std::vector<TrackId>{};
    ids.reserve(20);

    for (std::int32_t idx = 0; idx < 10; ++idx)
    {
      ids.push_back(env.lib.addTrack(makeSpec(std::string(1, static_cast<char>('J' - idx)), 2020)));
    }

    for (std::int32_t idx = 0; idx < 10; ++idx)
    {
      ids.push_back(env.lib.addTrack(makeSpec(std::string(1, static_cast<char>('J' - idx)), 2021)));
    }

    env.setupFiltered(ids);

    auto proj = env.createProjection(ViewId{1});
    auto const sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{.groupBy = TrackGroupKey::None,
                                               .sortBy = {
                                                 TrackSortTerm{.field = TrackSortField::Year, .ascending = true},
                                                 TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
                                               }});

    REQUIRE(proj.size() == 20);

    auto const checkOrder = [&](std::size_t start, std::size_t end, std::uint16_t expectedYear)
    {
      for (std::size_t idx = start; idx < end; ++idx)
      {
        auto txn = env.lib.library().readTransaction();
        auto reader = env.lib.library().tracks().reader(txn);
        auto const optV = reader.get(proj.trackIdAt(idx), TrackStore::Reader::LoadMode::Hot);

        CHECK(optV.has_value());
        CHECK(optV->metadata().year() == expectedYear);
      }
    };

    checkOrder(0, 10, 2020);
    checkOrder(10, 20, 2021);

    // Within each year group, titles are ascending
    for (auto const is2020 : {true, false})
    {
      std::size_t const start = is2020 ? std::size_t{0} : std::size_t{10};

      for (std::size_t idx = start; idx < start + 9; ++idx)
      {
        auto txn = env.lib.library().readTransaction();
        auto reader = env.lib.library().tracks().reader(txn);
        auto const optA = reader.get(proj.trackIdAt(idx), TrackStore::Reader::LoadMode::Hot);
        auto const optB = reader.get(proj.trackIdAt(idx + 1), TrackStore::Reader::LoadMode::Hot);

        REQUIRE(optA.has_value());
        REQUIRE(optB.has_value());
        CHECK(std::string{optA->metadata().title()} <= std::string{optB->metadata().title()});
      }
    }
  }

  TEST_CASE("TrackListProjection - sort 15 tracks by album disc track", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};
    auto ids = std::vector<TrackId>{};
    ids.reserve(20);
    struct Row final
    {
      std::string album;
      std::uint16_t disc;
      std::uint16_t track;
    };
    auto const rows = std::vector<Row>{
      {"Gamma", 1, 2},
      {"Alpha", 1, 3},
      {"Beta", 1, 2},
      {"Beta", 2, 4},
      {"Alpha", 1, 1},
      {"Beta", 2, 3},
      {"Alpha", 1, 5},
      {"Gamma", 1, 1},
      {"Alpha", 1, 2},
      {"Beta", 1, 1},
      {"Alpha", 1, 4},
      {"Beta", 2, 2},
      {"Gamma", 1, 3},
      {"Beta", 2, 1},
      {"Gamma", 3, 1},
    };

    ids.reserve(rows.size());

    for (auto const& r : rows)
    {
      auto spec = TrackSpec{};
      spec.album = r.album;
      spec.discNumber = r.disc;
      spec.trackNumber = r.track;
      ids.push_back(env.lib.addTrack(spec));
    }

    env.setupFiltered(ids);

    auto proj = env.createProjection(ViewId{1});
    auto const sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{.groupBy = TrackGroupKey::None,
                                               .sortBy = {
                                                 TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                                                 TrackSortTerm{.field = TrackSortField::DiscNumber, .ascending = true},
                                                 TrackSortTerm{.field = TrackSortField::TrackNumber, .ascending = true},
                                               }});

    REQUIRE(proj.size() == 15);

    // Verify monotonic: album non-decreasing, disc non-decreasing within album,
    // track non-decreasing within disc.
    auto prevAlbum = std::string{};
    std::uint16_t prevDisc = 0;
    std::uint16_t prevTrack = 0;

    auto const& dict = env.lib.library().dictionary();
    auto txn = env.lib.library().readTransaction();
    auto reader = env.lib.library().tracks().reader(txn);

    for (std::size_t i = 0; i < 15; ++i)
    {
      auto const optView = reader.get(proj.trackIdAt(i), TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView.has_value());

      auto const album = std::string{dict.get(optView->metadata().albumId())};
      auto const disc = optView->metadata().discNumber();
      auto const track = optView->metadata().trackNumber();

      if (i > 0)
      {
        auto const albumCmp = prevAlbum.compare(album);
        CHECK((albumCmp < 0 || (albumCmp == 0 && disc >= prevDisc) ||
               (albumCmp == 0 && disc == prevDisc && track >= prevTrack)));
      }

      prevAlbum = album;
      prevDisc = disc;
      prevTrack = track;
    }
  }

  TEST_CASE("TrackListProjection - sort 10 identical tracks preserves stability", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};
    auto ids = std::vector<TrackId>{};
    ids.reserve(10);

    for (std::int32_t i = 0; i < 10; ++i)
    {
      ids.push_back(env.lib.addTrack(makeSpec("Same", 2020)));
    }

    env.setupFiltered(ids);

    auto proj = env.createProjection(ViewId{1});
    auto const sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(
      TrackPresentationSpec{.groupBy = TrackGroupKey::None, .sortBy = {TrackSortTerm{.field = TrackSortField::Year}}});

    REQUIRE(proj.size() == 10);

    for (std::size_t i = 0; i < 10; ++i)
    {
      CHECK(proj.trackIdAt(i) != kInvalidTrackId);
    }
  }

  TEST_CASE("TrackListProjection - sort reversal 10 tracks avoids IO", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};
    auto ids = std::vector<TrackId>{};
    ids.reserve(10);

    for (std::int32_t y = 2019; y >= 2010; --y)
    {
      ids.push_back(env.lib.addTrack(makeSpec(std::format("{}", y), static_cast<std::uint16_t>(y))));
    }

    env.setupFiltered(ids);

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{
      .groupBy = TrackGroupKey::None, .sortBy = {TrackSortTerm{.field = TrackSortField::Year, .ascending = true}}});

    auto checkMonotonic = [&](bool ascending)
    {
      for (std::size_t i = 0; i < 9; ++i)
      {
        auto txn = env.lib.library().readTransaction();
        auto reader = env.lib.library().tracks().reader(txn);
        auto optA = reader.get(proj.trackIdAt(i), TrackStore::Reader::LoadMode::Hot);
        auto optB = reader.get(proj.trackIdAt(i + 1), TrackStore::Reader::LoadMode::Hot);
        REQUIRE(optA.has_value());
        REQUIRE(optB.has_value());
        auto ay = optA->metadata().year();

        if (auto by = optB->metadata().year(); ascending)
        {
          CHECK(ay <= by);
        }
        else
        {
          CHECK(ay >= by);
        }
      }
    };

    REQUIRE(proj.size() == 10);
    checkMonotonic(true);

    proj.setPresentation(TrackPresentationSpec{
      .groupBy = TrackGroupKey::None, .sortBy = {TrackSortTerm{.field = TrackSortField::Year, .ascending = false}}});
    checkMonotonic(false);

    proj.setPresentation(TrackPresentationSpec{
      .groupBy = TrackGroupKey::None, .sortBy = {TrackSortTerm{.field = TrackSortField::Year, .ascending = true}}});
    checkMonotonic(true);
  }

  TEST_CASE("TrackListProjection - switch year to title sort on 15 tracks", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};
    auto ids = std::vector<TrackId>{};
    ids.reserve(15);

    for (std::int32_t i = 0; i < 15; ++i)
    {
      auto title = std::string(1, static_cast<char>('A' + ((i * 7) % 15)));
      ids.push_back(env.lib.addTrack(makeSpec(title, static_cast<std::uint16_t>(2000 + ((i * 3) % 20)))));
    }

    env.setupFiltered(ids);

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{
      .groupBy = TrackGroupKey::None, .sortBy = {TrackSortTerm{.field = TrackSortField::Year, .ascending = true}}});
    REQUIRE(proj.size() == 15);

    for (std::size_t i = 0; i < 14; ++i)
    {
      auto txn = env.lib.library().readTransaction();
      auto reader = env.lib.library().tracks().reader(txn);
      auto optA = reader.get(proj.trackIdAt(i), TrackStore::Reader::LoadMode::Hot);
      auto optB = reader.get(proj.trackIdAt(i + 1), TrackStore::Reader::LoadMode::Hot);
      REQUIRE(optA.has_value());
      REQUIRE(optB.has_value());
      CHECK(optA->metadata().year() <= optB->metadata().year());
    }

    proj.setPresentation(TrackPresentationSpec{
      .groupBy = TrackGroupKey::None, .sortBy = {TrackSortTerm{.field = TrackSortField::Title, .ascending = true}}});
    REQUIRE(proj.size() == 15);

    for (std::size_t i = 0; i < 14; ++i)
    {
      auto txn = env.lib.library().readTransaction();
      auto reader = env.lib.library().tracks().reader(txn);
      auto optA = reader.get(proj.trackIdAt(i), TrackStore::Reader::LoadMode::Hot);
      auto optB = reader.get(proj.trackIdAt(i + 1), TrackStore::Reader::LoadMode::Hot);
      REQUIRE(optA.has_value());
      REQUIRE(optB.has_value());
      CHECK(std::string{optA->metadata().title()} <= std::string{optB->metadata().title()});
    }
  }

  TEST_CASE("TrackListProjection - group sections for Artist grouping", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};

    auto id1 = env.lib.addTrack(TrackSpec{.title = "T1", .artist = "Zeppelin", .album = "IV", .trackNumber = 1});
    auto id2 = env.lib.addTrack(TrackSpec{.title = "T2", .artist = "Zeppelin", .album = "IV", .trackNumber = 2});
    auto id3 = env.lib.addTrack(TrackSpec{.title = "T3", .artist = "Abba", .album = "Gold", .trackNumber = 1});
    auto id4 = env.lib.addTrack(TrackSpec{.title = "T4", .artist = "Coldplay", .album = "X", .trackNumber = 1});
    auto id5 = env.lib.addTrack(TrackSpec{.title = "T5", .artist = "Coldplay", .album = "Y", .trackNumber = 1});
    env.setupFiltered({{id1, id2, id3, id4, id5}});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{.groupBy = TrackGroupKey::Artist,
                                               .sortBy = {
                                                 TrackSortTerm{.field = TrackSortField::Artist, .ascending = true},
                                                 TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                                                 TrackSortTerm{.field = TrackSortField::TrackNumber, .ascending = true},
                                               }});

    REQUIRE(proj.size() == 5);

    // After sorting: Abba(Gold) < Coldplay(X) < Coldplay(Y) < Zeppelin(IV,T1) < Zeppelin(IV,T2)
    // Groups: Abba(1), Coldplay(2), Zeppelin(2)
    REQUIRE(proj.groupCount() == 3);

    auto s0 = proj.groupAt(0);
    CHECK(s0.label == "Abba");
    CHECK(s0.rows.start == 0);
    CHECK(s0.rows.count == 1);

    auto s1 = proj.groupAt(1);
    CHECK(s1.label == "Coldplay");
    CHECK(s1.rows.start == 1);
    CHECK(s1.rows.count == 2);

    auto s2 = proj.groupAt(2);
    CHECK(s2.label == "Zeppelin");
    CHECK(s2.rows.start == 3);
    CHECK(s2.rows.count == 2);

    CHECK(proj.groupIndexAt(0) == 0);
    CHECK(proj.groupIndexAt(1) == 1);
    CHECK(proj.groupIndexAt(2) == 1);
    CHECK(proj.groupIndexAt(3) == 2);
    CHECK(proj.groupIndexAt(4) == 2);
  }

  TEST_CASE("TrackListProjection - large artist groups keep compound sort order", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};

    auto ids = std::vector<TrackId>{};
    auto const add =
      [&](std::string title, std::string artist, std::string album, std::uint16_t disc, std::uint16_t track) -> TrackId
    {
      auto const id = env.lib.addTrack(TrackSpec{.title = std::move(title),
                                                 .artist = std::move(artist),
                                                 .album = std::move(album),
                                                 .discNumber = disc,
                                                 .trackNumber = track});
      ids.push_back(id);
      return id;
    };

    auto const zeppelinPhysicalDisc2Track2 = add("Kashmir", "Zeppelin", "Physical Graffiti", 2, 2);
    auto const coldplayXyTrack2 = add("Low", "Coldplay", "X&Y", 1, 2);
    auto const abbaGoldTrack2 = add("Dancing Queen", "Abba", "Gold", 1, 2);
    auto const radioheadOkTrack3 = add("Exit Music", "Radiohead", "OK Computer", 1, 3);
    auto const daftDiscoveryTrack2 = add("Aerodynamic", "Daft Punk", "Discovery", 1, 2);
    auto const zeppelinIvTrack1 = add("Black Dog", "Zeppelin", "IV", 1, 1);
    auto const abbaVoyageTrack1 = add("I Still Have Faith In You", "Abba", "Voyage", 1, 1);
    auto const coldplayParachutesTrack1 = add("Don't Panic", "Coldplay", "Parachutes", 1, 1);
    auto const radioheadKidTrack2 = add("Kid A", "Radiohead", "Kid A", 1, 2);
    auto const daftRandomTrack1 = add("Give Life Back To Music", "Daft Punk", "Random Access Memories", 1, 1);
    auto const abbaGoldTrack1 = add("Waterloo", "Abba", "Gold", 1, 1);
    auto const coldplayXyDisc2Track1 = add("Til Kingdom Come", "Coldplay", "X&Y", 2, 1);
    auto const radioheadOkTrack1 = add("Airbag", "Radiohead", "OK Computer", 1, 1);
    auto const zeppelinPhysicalDisc1Track1 = add("Custard Pie", "Zeppelin", "Physical Graffiti", 1, 1);
    auto const daftDiscoveryTrack1 = add("One More Time", "Daft Punk", "Discovery", 1, 1);
    auto const abbaVoyageTrack2 = add("When You Danced With Me", "Abba", "Voyage", 1, 2);
    auto const coldplayParachutesTrack2 = add("Shiver", "Coldplay", "Parachutes", 1, 2);
    auto const radioheadKidTrack1 = add("Everything In Its Right Place", "Radiohead", "Kid A", 1, 1);
    auto const zeppelinIvTrack2 = add("Rock and Roll", "Zeppelin", "IV", 1, 2);
    auto const daftDiscoveryTrack3 = add("Digital Love", "Daft Punk", "Discovery", 1, 3);
    auto const abbaGoldTrack5 = add("Mamma Mia", "Abba", "Gold", 1, 5);
    auto const coldplayXyTrack1 = add("Square One", "Coldplay", "X&Y", 1, 1);
    auto const radioheadOkTrack2 = add("Paranoid Android", "Radiohead", "OK Computer", 1, 2);
    auto const zeppelinPhysicalDisc2Track1 = add("In The Light", "Zeppelin", "Physical Graffiti", 2, 1);

    env.setupFiltered(ids);

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{.groupBy = TrackGroupKey::Artist,
                                               .sortBy = {
                                                 TrackSortTerm{.field = TrackSortField::Artist, .ascending = true},
                                                 TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                                                 TrackSortTerm{.field = TrackSortField::DiscNumber, .ascending = true},
                                                 TrackSortTerm{.field = TrackSortField::TrackNumber, .ascending = true},
                                                 TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
                                               }});

    auto const expectedOrder = std::vector{
      abbaGoldTrack1,
      abbaGoldTrack2,
      abbaGoldTrack5,
      abbaVoyageTrack1,
      abbaVoyageTrack2,
      coldplayParachutesTrack1,
      coldplayParachutesTrack2,
      coldplayXyTrack1,
      coldplayXyTrack2,
      coldplayXyDisc2Track1,
      daftDiscoveryTrack1,
      daftDiscoveryTrack2,
      daftDiscoveryTrack3,
      daftRandomTrack1,
      radioheadKidTrack1,
      radioheadKidTrack2,
      radioheadOkTrack1,
      radioheadOkTrack2,
      radioheadOkTrack3,
      zeppelinIvTrack1,
      zeppelinIvTrack2,
      zeppelinPhysicalDisc1Track1,
      zeppelinPhysicalDisc2Track1,
      zeppelinPhysicalDisc2Track2,
    };

    REQUIRE(proj.size() == expectedOrder.size());

    for (std::size_t index = 0; index < expectedOrder.size(); ++index)
    {
      CHECK(proj.trackIdAt(index) == expectedOrder[index]);
      CHECK(proj.indexOf(expectedOrder[index]) == index);
    }

    struct ExpectedGroup final
    {
      std::string_view label;
      std::size_t start;
      std::size_t count;
    };

    auto const expectedGroups = std::array{
      ExpectedGroup{.label = "Abba", .start = 0, .count = 5},
      ExpectedGroup{.label = "Coldplay", .start = 5, .count = 5},
      ExpectedGroup{.label = "Daft Punk", .start = 10, .count = 4},
      ExpectedGroup{.label = "Radiohead", .start = 14, .count = 5},
      ExpectedGroup{.label = "Zeppelin", .start = 19, .count = 5},
    };

    REQUIRE(proj.groupCount() == expectedGroups.size());

    for (std::size_t groupIndex = 0; groupIndex < expectedGroups.size(); ++groupIndex)
    {
      auto const group = proj.groupAt(groupIndex);
      CHECK(group.label == expectedGroups.at(groupIndex).label);
      CHECK(group.rows.start == expectedGroups.at(groupIndex).start);
      CHECK(group.rows.count == expectedGroups.at(groupIndex).count);

      for (std::size_t row = group.rows.start; row < group.rows.start + group.rows.count; ++row)
      {
        CHECK(proj.groupIndexAt(row) == groupIndex);
      }
    }

    CHECK_FALSE(proj.groupIndexAt(expectedOrder.size()).has_value());
  }

  TEST_CASE("TrackListProjection - group sections empty for None grouping", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};
    auto id1 = env.lib.addTrack(makeSpec("T1", 2020));
    env.setupFiltered({{id1}});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{.groupBy = TrackGroupKey::None,
                                               .sortBy = {
                                                 TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
                                               }});

    CHECK(proj.groupCount() == 0);
    auto s = proj.groupAt(0);
    CHECK(s.rows.count == 0);
    CHECK(s.label.empty());
    CHECK_FALSE(proj.groupIndexAt(0).has_value());
  }

  TEST_CASE("TrackListProjection - empty projection group metadata", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};
    env.setupFiltered({});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{.groupBy = TrackGroupKey::Artist,
                                               .sortBy = {
                                                 TrackSortTerm{.field = TrackSortField::Artist, .ascending = true},
                                               }});

    CHECK(proj.size() == 0);
    CHECK(proj.groupCount() == 0);
  }

  TEST_CASE("TrackListProjection - group label for unknown artist", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};

    auto id1 = env.lib.addTrack(TrackSpec{.title = "T1", .artist = "", .album = "A"});
    env.setupFiltered({{id1}});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{.groupBy = TrackGroupKey::Artist,
                                               .sortBy = {
                                                 TrackSortTerm{.field = TrackSortField::Artist, .ascending = true},
                                               }});

    REQUIRE(proj.size() == 1);
    REQUIRE(proj.groupCount() == 1);
    CHECK(proj.groupAt(0).label == "Unknown Artist");
  }

  TEST_CASE("TrackListProjection - group label for unknown year", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};

    auto id1 = env.lib.addTrack(TrackSpec{.title = "T1", .year = 0});
    env.setupFiltered({{id1}});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{.groupBy = TrackGroupKey::Year,
                                               .sortBy = {
                                                 TrackSortTerm{.field = TrackSortField::Year, .ascending = true},
                                               }});

    REQUIRE(proj.size() == 1);
    REQUIRE(proj.groupCount() == 1);
    CHECK(proj.groupAt(0).label == "Unknown Year");
  }

  TEST_CASE("TrackListProjection - album groups split by album artist", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};

    // Same album title, different album artists
    auto id1 = env.lib.addTrack(
      TrackSpec{.title = "T1", .artist = "Ari", .album = "Greatest Hits", .albumArtist = "Artist One"});
    auto id2 = env.lib.addTrack(
      TrackSpec{.title = "T2", .artist = "Ari", .album = "Greatest Hits", .albumArtist = "Artist Two"});
    env.setupFiltered({{id1, id2}});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{.groupBy = TrackGroupKey::Album,
                                               .sortBy = {
                                                 TrackSortTerm{.field = TrackSortField::AlbumArtist, .ascending = true},
                                                 TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                                               }});

    REQUIRE(proj.size() == 2);
    REQUIRE(proj.groupCount() == 2);
    CHECK(proj.groupAt(0).label == "Greatest Hits - Artist One");
    CHECK(proj.groupAt(1).label == "Greatest Hits - Artist Two");
  }

  TEST_CASE("TrackListProjection - presentation() returns correct snapshot", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};
    env.setupFiltered({});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{.groupBy = TrackGroupKey::Genre,
                                               .sortBy = {
                                                 TrackSortTerm{.field = TrackSortField::Genre, .ascending = true},
                                                 TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
                                               }});

    auto snap = proj.presentation();
    CHECK(snap.groupBy == TrackGroupKey::Genre);
    REQUIRE(snap.sortBy.size() == 2);
    CHECK(snap.sortBy[0].field == TrackSortField::Genre);
    CHECK(snap.sortBy[1].field == TrackSortField::Title);

    auto expectedRedundant = std::vector{TrackField::Genre};
    CHECK(snap.redundantFields == expectedRedundant);
  }

  TEST_CASE("TrackListProjection - group label for album without album artist", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};

    auto id1 = env.lib.addTrack(TrackSpec{.title = "T1", .album = "Solo Album", .albumArtist = ""});
    env.setupFiltered({{id1}});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{.groupBy = TrackGroupKey::Album,
                                               .sortBy = {
                                                 TrackSortTerm{.field = TrackSortField::AlbumArtist, .ascending = true},
                                                 TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                                               }});

    REQUIRE(proj.size() == 1);
    REQUIRE(proj.groupCount() == 1);
    CHECK(proj.groupAt(0).label == "Solo Album");
  }

  TEST_CASE("TrackListProjection - unknown album label", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};

    auto id1 = env.lib.addTrack(TrackSpec{.title = "T1", .album = "", .albumArtist = ""});
    env.setupFiltered({{id1}});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{.groupBy = TrackGroupKey::Album,
                                               .sortBy = {
                                                 TrackSortTerm{.field = TrackSortField::AlbumArtist, .ascending = true},
                                                 TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                                               }});

    REQUIRE(proj.size() == 1);
    REQUIRE(proj.groupCount() == 1);
    CHECK(proj.groupAt(0).label == "Unknown Album");
  }

  TEST_CASE("TrackListProjection - incremental batch insertion", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};
    auto id1 = env.lib.addTrack(makeSpec("B", 2020));
    auto id2 = env.lib.addTrack(makeSpec("D", 2020));
    env.setupFiltered({{id1, id2}});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{
      .groupBy = TrackGroupKey::None, .sortBy = {TrackSortTerm{.field = TrackSortField::Title, .ascending = true}}});

    REQUIRE(proj.size() == 2);
    CHECK(proj.trackIdAt(0) == id1);
    CHECK(proj.trackIdAt(1) == id2);

    auto id3 = env.lib.addTrack(makeSpec("A", 2020));
    auto id4 = env.lib.addTrack(makeSpec("C", 2020));
    auto id5 = env.lib.addTrack(makeSpec("E", 2020));

    // Simulate batch insertion from source
    // In our TestEnv, the filtered (SmartListSource) is the one attached to proj.
    // We notify the evaluator about the new tracks in the base source.
    env.source.batchInsert({{id3, id4, id5}});
    // SmartListSource reacts to source changes.

    // After updates, the projection should have updated incrementally
    REQUIRE(proj.size() == 5);
    CHECK(proj.trackIdAt(0) == id3); // A
    CHECK(proj.trackIdAt(1) == id1); // B
    CHECK(proj.trackIdAt(2) == id4); // C
    CHECK(proj.trackIdAt(3) == id2); // D
    CHECK(proj.trackIdAt(4) == id5); // E

    // Verify positionIndex
    CHECK(proj.indexOf(id3) == 0);
    CHECK(proj.indexOf(id1) == 1);
    CHECK(proj.indexOf(id4) == 2);
    CHECK(proj.indexOf(id2) == 3);
    CHECK(proj.indexOf(id5) == 4);
  }

  TEST_CASE("TrackListProjection - incremental batch removal", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};
    auto id1 = env.lib.addTrack(makeSpec("A", 2020));
    auto id2 = env.lib.addTrack(makeSpec("B", 2020));
    auto id3 = env.lib.addTrack(makeSpec("C", 2020));
    auto id4 = env.lib.addTrack(makeSpec("D", 2020));
    env.setupFiltered({{id1, id2, id3, id4}});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{
      .groupBy = TrackGroupKey::None, .sortBy = {TrackSortTerm{.field = TrackSortField::Title, .ascending = true}}});

    REQUIRE(proj.size() == 4);

    // Remove B and D
    // In our improved MutableTrackSource, we can now use batchRemove
    // But since filtered is a SmartListSource, it will handle the removal notification.
    env.source.batchRemove({{id2, id4}});

    REQUIRE(proj.size() == 2);
    CHECK(proj.trackIdAt(0) == id1);
    CHECK(proj.trackIdAt(1) == id3);

    CHECK(proj.indexOf(id1) == 0);
    CHECK(proj.indexOf(id3) == 1);
    CHECK_FALSE(proj.indexOf(id2).has_value());
    CHECK_FALSE(proj.indexOf(id4).has_value());
  }

  TEST_CASE("TrackListProjection - single and batch mutations without grouping", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};
    auto id1 = env.lib.addTrack(makeSpec("A", 2020));
    auto id2 = env.lib.addTrack(makeSpec("C", 2020));
    env.setupFiltered({{id1, id2}});

    auto proj = env.createProjection(ViewId{1});
    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    auto sub = proj.subscribe([&](TrackListProjectionDeltaBatch const& batch) { batches.push_back(batch); });

    // No grouping, yes comparator
    proj.setPresentation(TrackPresentationSpec{
      .groupBy = TrackGroupKey::None, .sortBy = {TrackSortTerm{.field = TrackSortField::Title, .ascending = true}}});
    batches.clear();

    SECTION("single insertion via single method")
    {
      auto id3 = env.lib.addTrack(TrackSpec{.title = "B", .year = 2020});
      env.source.singleInsert(id3);
      REQUIRE(proj.size() == 3);
      CHECK(proj.trackIdAt(0) == id1);
      CHECK(proj.trackIdAt(1) == id3);
      CHECK(proj.trackIdAt(2) == id2);
      CHECK(proj.indexOf(id3) == 1);
      REQUIRE(batches.size() == 1);
      auto const* delta = std::get_if<ProjectionInsertRange>(&batches.back().deltas.front());
      REQUIRE(delta != nullptr);
      CHECK(delta->range.start == 1);
      CHECK(delta->range.count == 1);
    }

    SECTION("single insertion via batch method")
    {
      auto id3 = env.lib.addTrack(TrackSpec{.title = "B", .year = 2020});
      auto arr = std::array{id3};
      env.source.batchInsert(arr);
      REQUIRE(proj.size() == 3);
      CHECK(proj.trackIdAt(0) == id1);
      CHECK(proj.trackIdAt(1) == id3);
      CHECK(proj.trackIdAt(2) == id2);
      CHECK(proj.indexOf(id3) == 1);
      REQUIRE(batches.size() == 1);
      auto const* delta = std::get_if<ProjectionInsertRange>(&batches.back().deltas.front());
      REQUIRE(delta != nullptr);
      CHECK(delta->range.start == 1);
      CHECK(delta->range.count == 1);
    }

    SECTION("single removal via single method")
    {
      env.source.singleRemove(id1);
      REQUIRE(proj.size() == 1);
      CHECK(proj.trackIdAt(0) == id2);
      CHECK_FALSE(proj.indexOf(id1).has_value());
      CHECK(proj.indexOf(id2) == 0);
      REQUIRE(batches.size() == 1);
      auto const* delta = std::get_if<ProjectionRemoveRange>(&batches.back().deltas.front());
      REQUIRE(delta != nullptr);
      CHECK(delta->range.start == 0);
      CHECK(delta->range.count == 1);
    }

    SECTION("single removal via batch method")
    {
      auto arr = std::array{id1};
      env.source.batchRemove(arr);
      REQUIRE(proj.size() == 1);
      CHECK(proj.trackIdAt(0) == id2);
      CHECK_FALSE(proj.indexOf(id1).has_value());
      CHECK(proj.indexOf(id2) == 0);
      REQUIRE(batches.size() == 1);
      auto const* delta = std::get_if<ProjectionRemoveRange>(&batches.back().deltas.front());
      REQUIRE(delta != nullptr);
      CHECK(delta->range.start == 0);
      CHECK(delta->range.count == 1);
    }

    SECTION("single update of non-sort field preserves order")
    {
      env.lib.updateTrack(id1, [](TrackSpec& s) { s.artist = "Updated Artist"; });
      env.source.singleUpdate(id1);
      REQUIRE(proj.size() == 2);
      CHECK(proj.trackIdAt(0) == id1);
      CHECK(proj.trackIdAt(1) == id2);
      REQUIRE(batches.size() == 1);
      auto const* delta = std::get_if<ProjectionUpdateRange>(&batches.back().deltas.front());
      REQUIRE(delta != nullptr);
      CHECK(delta->range.start == 0);
      CHECK(delta->range.count == 1);
    }

    SECTION("single update breaking order")
    {
      // A -> Z
      env.lib.updateTrack(id1, [](TrackSpec& s) { s.title = "Z"; });
      env.source.singleUpdate(id1);
      REQUIRE(proj.size() == 2);
      CHECK(proj.trackIdAt(0) == id2); // C
      CHECK(proj.trackIdAt(1) == id1); // Z
      CHECK(proj.indexOf(id1) == 1);
      REQUIRE(batches.size() == 2);
      CHECK(std::holds_alternative<ProjectionRemoveRange>(batches[0].deltas.front()));
      CHECK(std::holds_alternative<ProjectionInsertRange>(batches[1].deltas.front()));
    }

    SECTION("single update via batch method")
    {
      env.lib.updateTrack(id1, [](TrackSpec& s) { s.artist = "Updated Artist"; });
      auto arr = std::array{id1};
      env.source.batchUpdate(arr);
      REQUIRE(proj.size() == 2);
      CHECK(proj.trackIdAt(0) == id1);
      CHECK(proj.trackIdAt(1) == id2);
      REQUIRE(batches.size() == 1);
      CHECK(std::holds_alternative<ProjectionUpdateRange>(batches.back().deltas.front()));
    }

    SECTION("batch insertion with grouping")
    {
      proj.setPresentation(TrackPresentationSpec{
        .groupBy = TrackGroupKey::Genre, .sortBy = {TrackSortTerm{.field = TrackSortField::Genre, .ascending = true}}});
      batches.clear();

      auto id3 = env.lib.addTrack(TrackSpec{.title = "B", .genre = "Pop", .year = 2020});
      auto id4 = env.lib.addTrack(TrackSpec{.title = "D", .genre = "Rock", .year = 2020});
      auto arr = std::array{id3, id4};
      env.source.batchInsert(arr);
      REQUIRE(proj.size() == 4);
      REQUIRE(proj.groupCount() == 3);
      CHECK(proj.groupAt(0).label == "Unknown Genre");
      CHECK(proj.groupAt(1).label == "Pop");
      CHECK(proj.groupAt(2).label == "Rock");
      REQUIRE(batches.size() == 1);
      CHECK(std::holds_alternative<ProjectionReset>(batches.back().deltas.front()));
    }

    SECTION("batch removal with grouping")
    {
      proj.setPresentation(
        TrackPresentationSpec{.groupBy = TrackGroupKey::Composer,
                              .sortBy = {TrackSortTerm{.field = TrackSortField::Composer, .ascending = true}}});
      batches.clear();

      auto arr = std::array{id1, id2};
      env.source.batchRemove(arr);
      REQUIRE(proj.size() == 0);
      CHECK(proj.groupCount() == 0);
      REQUIRE(batches.size() == 1);
      CHECK(std::holds_alternative<ProjectionReset>(batches.back().deltas.front()));
    }

    SECTION("batch update with grouping")
    {
      proj.setPresentation(
        TrackPresentationSpec{.groupBy = TrackGroupKey::AlbumArtist,
                              .sortBy = {TrackSortTerm{.field = TrackSortField::AlbumArtist, .ascending = true}}});
      batches.clear();

      auto arr = std::array{id1, id2};
      env.source.batchUpdate(arr);
      REQUIRE(proj.size() == 2);
      REQUIRE(proj.groupCount() == 1);
      CHECK(proj.groupAt(0).label == "Unknown Artist");
      REQUIRE(batches.size() == 1);
      CHECK(std::holds_alternative<ProjectionReset>(batches.back().deltas.front()));
    }

    SECTION("single insertion with grouping")
    {
      proj.setPresentation(TrackPresentationSpec{
        .groupBy = TrackGroupKey::Genre, .sortBy = {TrackSortTerm{.field = TrackSortField::Genre, .ascending = true}}});
      batches.clear();

      auto id3 = env.lib.addTrack(TrackSpec{.title = "B", .genre = "Pop", .year = 2020});
      env.source.singleInsert(id3);
      REQUIRE(proj.size() == 3);
      REQUIRE(proj.groupCount() == 2);
      CHECK(proj.groupAt(0).label == "Unknown Genre");
      CHECK(proj.groupAt(1).label == "Pop");
      REQUIRE(batches.size() == 1);
      CHECK(std::holds_alternative<ProjectionReset>(batches.back().deltas.front()));
    }

    SECTION("single removal with grouping")
    {
      proj.setPresentation(TrackPresentationSpec{
        .groupBy = TrackGroupKey::Genre, .sortBy = {TrackSortTerm{.field = TrackSortField::Genre, .ascending = true}}});
      batches.clear();

      env.source.singleRemove(id1);
      REQUIRE(proj.size() == 1);
      CHECK(proj.trackIdAt(0) == id2);
      REQUIRE(proj.groupCount() == 1);
      CHECK(proj.groupAt(0).label == "Unknown Genre");
      REQUIRE(batches.size() == 1);
      CHECK(std::holds_alternative<ProjectionReset>(batches.back().deltas.front()));
    }

    SECTION("batch insert without comparator")
    {
      proj.setPresentation(TrackPresentationSpec{.groupBy = TrackGroupKey::None});
      batches.clear();

      auto id3 = env.lib.addTrack(TrackSpec{.title = "B", .year = 2020});
      auto id4 = env.lib.addTrack(TrackSpec{.title = "D", .year = 2020});
      auto arr = std::array{id3, id4};
      env.source.batchInsert(arr);
      REQUIRE(proj.size() == 4);
      CHECK(proj.trackIdAt(0) == id1);
      CHECK(proj.trackIdAt(1) == id2);
      CHECK(proj.trackIdAt(2) == id3);
      CHECK(proj.trackIdAt(3) == id4);
      REQUIRE(batches.size() == 1);
      CHECK(std::holds_alternative<ProjectionReset>(batches.back().deltas.front()));
    }

    SECTION("single update without comparator")
    {
      proj.setPresentation(TrackPresentationSpec{.groupBy = TrackGroupKey::None});
      batches.clear();

      env.lib.updateTrack(id1, [](TrackSpec& s) { s.title = "AA"; });
      env.source.singleUpdate(id1);
      REQUIRE(proj.size() == 2);
      CHECK(proj.trackIdAt(0) == id1);
      CHECK(proj.trackIdAt(1) == id2);
      REQUIRE(batches.size() == 1);
      auto const* delta = std::get_if<ProjectionUpdateRange>(&batches.back().deltas.front());
      REQUIRE(delta != nullptr);
      CHECK(delta->range.start == 0);
      CHECK(delta->range.count == 1);
    }

    SECTION("Destructor coverage")
    {
      auto proj2 = std::make_unique<TrackListProjection>(ViewId{2}, env.source, env.lib.library());
      proj2.reset();
    }
  }

  TEST_CASE("TrackListProjection - sorting and grouping by specific fields", "[app][unit][runtime][projection]")
  {
    auto env = TestEnv{};

    auto id1 = env.lib.addTrack(
      TrackSpec{.title = "A", .genre = "Pop", .composer = "Mozart", .work = "Opus 1", .durationMs = 5000});
    auto id2 = env.lib.addTrack(
      TrackSpec{.title = "B", .genre = "Rock", .composer = "Bach", .work = "Opus 2", .durationMs = 3000});
    env.setupFiltered({{id1, id2}});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    SECTION("Sort by Duration")
    {
      proj.setPresentation(
        TrackPresentationSpec{.groupBy = TrackGroupKey::None,
                              .sortBy = {TrackSortTerm{.field = TrackSortField::Duration, .ascending = true}}});
      REQUIRE(proj.size() == 2);
      CHECK(proj.trackIdAt(0) == id2); // 3000ms
      CHECK(proj.trackIdAt(1) == id1); // 5000ms
    }

    SECTION("Group by Genre")
    {
      proj.setPresentation(TrackPresentationSpec{
        .groupBy = TrackGroupKey::Genre, .sortBy = {TrackSortTerm{.field = TrackSortField::Genre, .ascending = true}}});
      REQUIRE(proj.size() == 2);
      REQUIRE(proj.groupCount() == 2);
      CHECK(proj.groupAt(0).label == "Pop");
      CHECK(proj.groupAt(1).label == "Rock");
    }

    SECTION("Group by Composer")
    {
      proj.setPresentation(
        TrackPresentationSpec{.groupBy = TrackGroupKey::Composer,
                              .sortBy = {TrackSortTerm{.field = TrackSortField::Composer, .ascending = true}}});
      REQUIRE(proj.size() == 2);
      REQUIRE(proj.groupCount() == 2);
      CHECK(proj.groupAt(0).label == "Bach");
      CHECK(proj.groupAt(1).label == "Mozart");
    }

    SECTION("Group by Work")
    {
      proj.setPresentation(TrackPresentationSpec{
        .groupBy = TrackGroupKey::Work, .sortBy = {TrackSortTerm{.field = TrackSortField::Work, .ascending = true}}});
      REQUIRE(proj.size() == 2);
      REQUIRE(proj.groupCount() == 2);
      CHECK(proj.groupAt(0).label == "Opus 1");
      CHECK(proj.groupAt(1).label == "Opus 2");
    }
  }
} // namespace ao::rt::test
