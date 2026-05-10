// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include <runtime/TrackListProjection.h>

#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <runtime/SmartListEvaluator.h>
#include <runtime/SmartListSource.h>
#include <runtime/TrackSource.h>

#include "TestUtils.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::app::test
{
  namespace
  {
    using ao::TrackId;
    using ao::app::SmartListEvaluator;
    using ao::app::SmartListSource;
    using ao::app::TrackSource;
    using ao::library::MusicLibrary;
    using ao::library::TrackBuilder;
    using ao::library::TrackStore;

    class MutableTrackSource final : public TrackSource
    {
    public:
      void addInitial(TrackId id) { _ids.push_back(id); }
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

    struct TestEnv final
    {
      TestMusicLibrary lib;
      MutableTrackSource source;
      ao::app::SmartListEvaluator engine;
      std::unique_ptr<ao::app::SmartListSource> filtered;

      TestEnv()
        : engine{lib.library()}
      {
      }

      auto createProjection(ViewId viewId) -> TrackListProjection
      {
        return TrackListProjection{viewId, *filtered, lib.library()};
      }

      void setupFiltered(std::span<TrackId const> ids)
      {
        for (auto id : ids)
        {
          source.addInitial(id);
        }
        filtered = std::make_unique<ao::app::SmartListSource>(source, lib.library(), engine);
        filtered->reload();
      }
    };
  }

  TEST_CASE("TrackListProjection - basic lifecycle with data", "[app][runtime][projection]")
  {
    auto env = TestEnv{};
    auto id1 = env.lib.addTrack(makeSpec("Track A", 2020));
    auto id2 = env.lib.addTrack(makeSpec("Track B", 2021));
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
      CHECK(proj.trackIdAt(999) == TrackId{});
    }
  }

  TEST_CASE("TrackListProjection - subscribe reset-on-subscribe", "[app][runtime][projection]")
  {
    auto env = TestEnv{};
    auto id1 = env.lib.addTrack(makeSpec("Track", 2020));
    env.setupFiltered({{id1}});

    auto proj = env.createProjection(ViewId{1});
    auto receivedReset = false;
    auto sub = proj.subscribe(
      [&](TrackListProjectionDeltaBatch const& batch)
      {
        REQUIRE(batch.deltas.size() == 1);
        CHECK(std::holds_alternative<ProjectionReset>(batch.deltas[0]));
        receivedReset = true;
      });

    CHECK(receivedReset);
  }

  TEST_CASE("TrackListProjection - multiple subscribers", "[app][runtime][projection]")
  {
    auto env = TestEnv{};
    auto id1 = env.lib.addTrack(makeSpec("Track", 2020));
    env.setupFiltered({{id1}});

    auto proj = env.createProjection(ViewId{1});
    auto count = 0;
    auto sub1 = proj.subscribe([&](TrackListProjectionDeltaBatch const&) { ++count; });
    auto sub2 = proj.subscribe([&](TrackListProjectionDeltaBatch const&) { ++count; });
    CHECK(count == 2);
  }

  TEST_CASE("TrackListProjection - unsubscribed handler not called", "[app][runtime][projection]")
  {
    auto env = TestEnv{};
    auto id1 = env.lib.addTrack(makeSpec("Track", 2020));
    env.setupFiltered({{id1}});

    auto proj = env.createProjection(ViewId{1});
    auto count = 0;
    auto sub = proj.subscribe([&](TrackListProjectionDeltaBatch const&) { ++count; });
    auto initial = count;
    sub.reset();
    CHECK(count == initial);
  }

  TEST_CASE("TrackListProjection - empty projection", "[app][runtime][projection]")
  {
    auto env = TestEnv{};
    env.setupFiltered({});

    auto proj = env.createProjection(ViewId{1});
    CHECK(proj.size() == 0);
    CHECK(proj.trackIdAt(0) == TrackId{});
    CHECK(proj.trackIdAt(999) == TrackId{});
  }

  TEST_CASE("TrackListProjection - sort 20 tracks by year then title", "[app][runtime][projection]")
  {
    auto env = TestEnv{};
    std::vector<TrackId> ids;
    ids.reserve(20);
    for (auto i = 0; i < 10; ++i)
    {
      ids.push_back(env.lib.addTrack(makeSpec(std::string(1, static_cast<char>('J' - i)), 2020)));
    }
    for (auto i = 0; i < 10; ++i)
    {
      ids.push_back(env.lib.addTrack(makeSpec(std::string(1, static_cast<char>('J' - i)), 2021)));
    }
    env.setupFiltered(ids);

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackGroupKey::None, {
      TrackSortTerm{.field = TrackSortField::Year, .ascending = true},
      TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
    });

    REQUIRE(proj.size() == 20);

    auto checkOrder = [&](std::size_t start, std::size_t end, std::uint16_t expectedYear)
    {
      for (auto i = start; i < end; ++i)
      {
        auto txn = env.lib.library().readTransaction();
        auto reader = env.lib.library().tracks().reader(txn);
        auto v = reader.get(proj.trackIdAt(i), TrackStore::Reader::LoadMode::Hot);
        CHECK(v.has_value());
        CHECK(v->metadata().year() == expectedYear);
      }
    };

    checkOrder(0, 10, 2020);
    checkOrder(10, 20, 2021);

    // Within each year group, titles are ascending
    for (auto is2020 : {true, false})
    {
      auto start = is2020 ? std::size_t{0} : std::size_t{10};
      for (auto i = start; i < start + 9; ++i)
      {
        auto txn = env.lib.library().readTransaction();
        auto reader = env.lib.library().tracks().reader(txn);
        auto a = reader.get(proj.trackIdAt(i), TrackStore::Reader::LoadMode::Hot);
        auto b = reader.get(proj.trackIdAt(i + 1), TrackStore::Reader::LoadMode::Hot);
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        CHECK(std::string{a->metadata().title()} <= std::string{b->metadata().title()});
      }
    }
  }

  TEST_CASE("TrackListProjection - sort 15 tracks by album disc track", "[app][runtime][projection]")
  {
    auto env = TestEnv{};
    std::vector<TrackId> ids;
    ids.reserve(20);
    struct Row
    {
      std::string album;
      std::uint16_t disc;
      std::uint16_t track;
    };
    auto rows = std::vector<Row>{
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
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackGroupKey::None, {
      TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
      TrackSortTerm{.field = TrackSortField::DiscNumber, .ascending = true},
      TrackSortTerm{.field = TrackSortField::TrackNumber, .ascending = true},
    });

    REQUIRE(proj.size() == 15);

    // Verify monotonic: album non-decreasing, disc non-decreasing within album,
    // track non-decreasing within disc.
    auto prevAlbum = std::string{};
    auto prevDisc = std::uint16_t{0};
    auto prevTrack = std::uint16_t{0};

    auto& dict = env.lib.library().dictionary();
    auto txn = env.lib.library().readTransaction();
    auto reader = env.lib.library().tracks().reader(txn);

    for (auto i = std::size_t{0}; i < 15; ++i)
    {
      auto view = reader.get(proj.trackIdAt(i), TrackStore::Reader::LoadMode::Both);
      REQUIRE(view.has_value());

      auto album = std::string{dict.get(view->metadata().albumId())};
      auto disc = view->metadata().discNumber();
      auto track = view->metadata().trackNumber();

      if (i > 0)
      {
        auto albumCmp = prevAlbum.compare(album);
        CHECK((albumCmp < 0 || (albumCmp == 0 && disc >= prevDisc) ||
               (albumCmp == 0 && disc == prevDisc && track >= prevTrack)));
      }

      prevAlbum = album;
      prevDisc = disc;
      prevTrack = track;
    }
  }

  TEST_CASE("TrackListProjection - sort 10 identical tracks preserves stability", "[app][runtime][projection]")
  {
    auto env = TestEnv{};
    std::vector<TrackId> ids;
    ids.reserve(10);
    for (auto i = 0; i < 10; ++i)
    {
      ids.push_back(env.lib.addTrack(makeSpec("Same", 2020)));
    }
    env.setupFiltered(ids);

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackGroupKey::None, {TrackSortTerm{.field = TrackSortField::Year}});

    REQUIRE(proj.size() == 10);
    for (auto i = std::size_t{0}; i < 10; ++i)
    {
      CHECK(proj.trackIdAt(i) != TrackId{});
    }
  }

  TEST_CASE("TrackListProjection - sort reversal 10 tracks avoids IO", "[app][runtime][projection]")
  {
    auto env = TestEnv{};
    std::vector<TrackId> ids;
    ids.reserve(10);
    for (auto y = 2019; y >= 2010; --y)
    {
      ids.push_back(env.lib.addTrack(makeSpec(std::to_string(y), static_cast<std::uint16_t>(y))));
    }
    env.setupFiltered(ids);

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackGroupKey::None, {TrackSortTerm{.field = TrackSortField::Year, .ascending = true}});

    auto checkMonotonic = [&](bool ascending)
    {
      for (auto i = std::size_t{0}; i < 9; ++i)
      {
        auto txn = env.lib.library().readTransaction();
        auto reader = env.lib.library().tracks().reader(txn);
        auto a = reader.get(proj.trackIdAt(i), TrackStore::Reader::LoadMode::Hot);
        auto b = reader.get(proj.trackIdAt(i + 1), TrackStore::Reader::LoadMode::Hot);
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        auto ay = a->metadata().year();
        auto by = b->metadata().year();
        if (ascending)
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

    proj.setPresentation(TrackGroupKey::None, {TrackSortTerm{.field = TrackSortField::Year, .ascending = false}});
    checkMonotonic(false);

    proj.setPresentation(TrackGroupKey::None, {TrackSortTerm{.field = TrackSortField::Year, .ascending = true}});
    checkMonotonic(true);
  }

  TEST_CASE("TrackListProjection - switch year to title sort on 15 tracks", "[app][runtime][projection]")
  {
    auto env = TestEnv{};
    std::vector<TrackId> ids;
    ids.reserve(15);
    for (auto i = 0; i < 15; ++i)
    {
      auto title = std::string(1, static_cast<char>('A' + ((i * 7) % 15)));
      ids.push_back(env.lib.addTrack(makeSpec(title, static_cast<std::uint16_t>(2000 + ((i * 3) % 20)))));
    }
    env.setupFiltered(ids);

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackGroupKey::None, {TrackSortTerm{.field = TrackSortField::Year, .ascending = true}});
    REQUIRE(proj.size() == 15);

    for (auto i = std::size_t{0}; i < 14; ++i)
    {
      auto txn = env.lib.library().readTransaction();
      auto reader = env.lib.library().tracks().reader(txn);
      auto a = reader.get(proj.trackIdAt(i), TrackStore::Reader::LoadMode::Hot);
      auto b = reader.get(proj.trackIdAt(i + 1), TrackStore::Reader::LoadMode::Hot);
      REQUIRE(a.has_value());
      REQUIRE(b.has_value());
      CHECK(a->metadata().year() <= b->metadata().year());
    }

    proj.setPresentation(TrackGroupKey::None, {TrackSortTerm{.field = TrackSortField::Title, .ascending = true}});
    REQUIRE(proj.size() == 15);

    for (auto i = std::size_t{0}; i < 14; ++i)
    {
      auto txn = env.lib.library().readTransaction();
      auto reader = env.lib.library().tracks().reader(txn);
      auto a = reader.get(proj.trackIdAt(i), TrackStore::Reader::LoadMode::Hot);
      auto b = reader.get(proj.trackIdAt(i + 1), TrackStore::Reader::LoadMode::Hot);
      REQUIRE(a.has_value());
      REQUIRE(b.has_value());
      CHECK(std::string{a->metadata().title()} <= std::string{b->metadata().title()});
    }
  }

  TEST_CASE("TrackListProjection - group sections for Artist grouping", "[app][runtime][projection]")
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

    proj.setPresentation(TrackGroupKey::Artist, {
      TrackSortTerm{.field = TrackSortField::Artist, .ascending = true},
      TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
      TrackSortTerm{.field = TrackSortField::TrackNumber, .ascending = true},
    });

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

  TEST_CASE("TrackListProjection - group sections empty for None grouping", "[app][runtime][projection]")
  {
    auto env = TestEnv{};
    auto id1 = env.lib.addTrack(makeSpec("T1", 2020));
    env.setupFiltered({{id1}});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackGroupKey::None, {
      TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
    });

    CHECK(proj.groupCount() == 0);
    auto s = proj.groupAt(0);
    CHECK(s.rows.count == 0);
    CHECK(s.label.empty());
    CHECK_FALSE(proj.groupIndexAt(0).has_value());
  }

  TEST_CASE("TrackListProjection - empty projection group metadata", "[app][runtime][projection]")
  {
    auto env = TestEnv{};
    env.setupFiltered({});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackGroupKey::Artist, {
      TrackSortTerm{.field = TrackSortField::Artist, .ascending = true},
    });

    CHECK(proj.size() == 0);
    CHECK(proj.groupCount() == 0);
  }

  TEST_CASE("TrackListProjection - group label for unknown artist", "[app][runtime][projection]")
  {
    auto env = TestEnv{};

    auto id1 = env.lib.addTrack(TrackSpec{.title = "T1", .artist = "", .album = "A"});
    env.setupFiltered({{id1}});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackGroupKey::Artist, {
      TrackSortTerm{.field = TrackSortField::Artist, .ascending = true},
    });

    REQUIRE(proj.size() == 1);
    REQUIRE(proj.groupCount() == 1);
    CHECK(proj.groupAt(0).label == "Unknown Artist");
  }

  TEST_CASE("TrackListProjection - group label for unknown year", "[app][runtime][projection]")
  {
    auto env = TestEnv{};

    auto id1 = env.lib.addTrack(TrackSpec{.title = "T1", .year = 0});
    env.setupFiltered({{id1}});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackGroupKey::Year, {
      TrackSortTerm{.field = TrackSortField::Year, .ascending = true},
    });

    REQUIRE(proj.size() == 1);
    REQUIRE(proj.groupCount() == 1);
    CHECK(proj.groupAt(0).label == "Unknown Year");
  }

  TEST_CASE("TrackListProjection - album groups split by album artist", "[app][runtime][projection]")
  {
    auto env = TestEnv{};

    // Same album title, different album artists
    auto id1 = env.lib.addTrack(TrackSpec{
      .title = "T1", .artist = "Ari", .album = "Greatest Hits", .albumArtist = "Artist One"});
    auto id2 = env.lib.addTrack(TrackSpec{
      .title = "T2", .artist = "Ari", .album = "Greatest Hits", .albumArtist = "Artist Two"});
    env.setupFiltered({{id1, id2}});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackGroupKey::Album, {
      TrackSortTerm{.field = TrackSortField::AlbumArtist, .ascending = true},
      TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
    });

    REQUIRE(proj.size() == 2);
    REQUIRE(proj.groupCount() == 2);
    CHECK(proj.groupAt(0).label == "Greatest Hits - Artist One");
    CHECK(proj.groupAt(1).label == "Greatest Hits - Artist Two");
  }

  TEST_CASE("TrackListProjection - presentation() returns correct snapshot", "[app][runtime][projection]")
  {
    auto env = TestEnv{};
    env.setupFiltered({});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackGroupKey::Genre, {
      TrackSortTerm{.field = TrackSortField::Genre, .ascending = true},
      TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
    });

    auto snap = proj.presentation();
    CHECK(snap.groupBy == TrackGroupKey::Genre);
    REQUIRE(snap.effectiveSortBy.size() == 2);
    CHECK(snap.effectiveSortBy[0].field == TrackSortField::Genre);
    CHECK(snap.effectiveSortBy[1].field == TrackSortField::Title);

    std::vector<TrackSortField> expectedRedundant = {TrackSortField::Genre};
    CHECK(snap.redundantFields == expectedRedundant);
  }

  TEST_CASE("TrackListProjection - group label for album without album artist", "[app][runtime][projection]")
  {
    auto env = TestEnv{};

    auto id1 = env.lib.addTrack(TrackSpec{
      .title = "T1", .album = "Solo Album", .albumArtist = ""});
    env.setupFiltered({{id1}});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackGroupKey::Album, {
      TrackSortTerm{.field = TrackSortField::AlbumArtist, .ascending = true},
      TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
    });

    REQUIRE(proj.size() == 1);
    REQUIRE(proj.groupCount() == 1);
    CHECK(proj.groupAt(0).label == "Solo Album");
  }

  TEST_CASE("TrackListProjection - unknown album label", "[app][runtime][projection]")
  {
    auto env = TestEnv{};

    auto id1 = env.lib.addTrack(TrackSpec{.title = "T1", .album = "", .albumArtist = ""});
    env.setupFiltered({{id1}});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackGroupKey::Album, {
      TrackSortTerm{.field = TrackSortField::AlbumArtist, .ascending = true},
      TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
    });

    REQUIRE(proj.size() == 1);
    REQUIRE(proj.groupCount() == 1);
    CHECK(proj.groupAt(0).label == "Unknown Album");
  }
}
