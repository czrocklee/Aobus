// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/projection/TrackListProjectionTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/projection/TrackListProjection.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace ao::rt::test
{
  using ao::library::TrackStore;

  TEST_CASE("TrackListProjection - title sort ignores leading articles", "[runtime][unit][projection]")
  {
    auto env = TestEnv{};
    auto const id1 = env.lib.addTrack(library::test::TrackSpec{.title = "The Best"});
    auto const id2 = env.lib.addTrack(library::test::TrackSpec{.title = "A Better"});
    auto const id3 = env.lib.addTrack(library::test::TrackSpec{.title = "An Apple"});
    auto const id4 = env.lib.addTrack(library::test::TrackSpec{.title = "Zeppelin"});
    auto const id5 = env.lib.addTrack(library::test::TrackSpec{.title = "the other"});
    auto const id6 = env.lib.addTrack(library::test::TrackSpec{.title = "a different"});
    auto const id7 = env.lib.addTrack(library::test::TrackSpec{.title = "ANOTHER"});

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

  TEST_CASE("TrackListProjection - sort 20 tracks by year then title", "[runtime][unit][projection]")
  {
    auto env = TestEnv{};
    auto ids = std::vector<TrackId>{};
    ids.reserve(20);

    for (std::int32_t index = 0; index < 10; ++index)
    {
      ids.push_back(
        env.lib.addTrack(library::test::makeTrackSpec(std::string(1, static_cast<char>('J' - index)), 2020)));
    }

    for (std::int32_t index = 0; index < 10; ++index)
    {
      ids.push_back(
        env.lib.addTrack(library::test::makeTrackSpec(std::string(1, static_cast<char>('J' - index)), 2021)));
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
      for (std::size_t index = start; index < end; ++index)
      {
        auto transaction = env.lib.library().readTransaction();
        auto reader = env.lib.library().tracks().reader(transaction);
        auto const optV = reader.get(proj.trackIdAt(index), TrackStore::Reader::LoadMode::Hot);

        REQUIRE(optV);
        CHECK(optV->metadata().year() == expectedYear);
      }
    };

    checkOrder(0, 10, 2020);
    checkOrder(10, 20, 2021);

    // Within each year group, titles are ascending
    for (auto const is2020 : {true, false})
    {
      std::size_t const start = is2020 ? std::size_t{0} : std::size_t{10};

      for (std::size_t index = start; index < start + 9; ++index)
      {
        auto transaction = env.lib.library().readTransaction();
        auto reader = env.lib.library().tracks().reader(transaction);
        auto const optA = reader.get(proj.trackIdAt(index), TrackStore::Reader::LoadMode::Hot);
        auto const optB = reader.get(proj.trackIdAt(index + 1), TrackStore::Reader::LoadMode::Hot);

        REQUIRE(optA);
        REQUIRE(optB);
        CHECK(std::string{optA->metadata().title()} <= std::string{optB->metadata().title()});
      }
    }
  }

  TEST_CASE("TrackListProjection - sort 15 tracks by album disc track", "[runtime][unit][projection]")
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
      auto spec = library::test::TrackSpec{};
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
    auto previousAlbum = std::string{};
    std::uint16_t previousDisc = 0;
    std::uint16_t previousTrack = 0;

    auto const& dictionary = env.lib.library().dictionary();
    auto transaction = env.lib.library().readTransaction();
    auto reader = env.lib.library().tracks().reader(transaction);

    for (std::size_t i = 0; i < 15; ++i)
    {
      auto const optView = reader.get(proj.trackIdAt(i), TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);

      auto const album = std::string{dictionary.get(optView->metadata().albumId())};
      auto const disc = optView->metadata().discNumber();
      auto const track = optView->metadata().trackNumber();

      if (i > 0)
      {
        auto const albumCmp = previousAlbum.compare(album);
        CHECK((albumCmp < 0 || (albumCmp == 0 && disc >= previousDisc) ||
               (albumCmp == 0 && disc == previousDisc && track >= previousTrack)));
      }

      previousAlbum = album;
      previousDisc = disc;
      previousTrack = track;
    }
  }

  TEST_CASE("TrackListProjection - sorts by classical role metadata", "[runtime][unit][projection]")
  {
    auto env = TestEnv{};

    auto const id1 = env.lib.addTrack(library::test::TrackSpec{.title = "A",
                                                               .conductor = "Leonard Bernstein",
                                                               .ensemble = "New York Philharmonic",
                                                               .soloist = "Martha Argerich"});
    auto const id2 = env.lib.addTrack(library::test::TrackSpec{
      .title = "B", .conductor = "Carlos Kleiber", .ensemble = "Vienna Philharmonic", .soloist = "Yo-Yo Ma"});
    auto const id3 = env.lib.addTrack(library::test::TrackSpec{
      .title = "C", .conductor = "Carlos Kleiber", .ensemble = "Staatskapelle Dresden", .soloist = "Glenn Gould"});

    env.setupFiltered({{id1, id2, id3}});

    auto proj = env.createProjection(ViewId{1});
    auto const sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{.groupBy = TrackGroupKey::None,
                                               .sortBy = {
                                                 TrackSortTerm{.field = TrackSortField::Conductor, .ascending = true},
                                                 TrackSortTerm{.field = TrackSortField::Ensemble, .ascending = true},
                                                 TrackSortTerm{.field = TrackSortField::Soloist, .ascending = true},
                                               }});

    REQUIRE(proj.size() == 3);
    CHECK(proj.trackIdAt(0) == id3);
    CHECK(proj.trackIdAt(1) == id2);
    CHECK(proj.trackIdAt(2) == id1);
  }

  TEST_CASE("TrackListProjection - movement sort keeps performances contiguous", "[runtime][unit][projection]")
  {
    // One work recorded twice (two albums/performances). Grouping by Work merges both
    // performances into a single section. The classical sort order places Album before
    // Movement, so each performance's movements must stay contiguous and movement-ordered
    // rather than interleaving (Karajan-I, Kleiber-I, Karajan-II, ...).
    auto env = TestEnv{};

    struct Row final
    {
      std::string album;
      std::uint16_t movementNumber;
      std::uint16_t trackNumber;
    };

    // Deliberately shuffled input order.
    auto const rows = std::vector<Row>{
      {"Kleiber 1974", 2, 2},
      {"Karajan 1963", 3, 3},
      {"Kleiber 1974", 1, 1},
      {"Karajan 1963", 1, 1},
      {"Kleiber 1974", 3, 3},
      {"Karajan 1963", 2, 2},
    };

    auto ids = std::vector<TrackId>{};
    ids.reserve(rows.size());

    for (auto const& r : rows)
    {
      auto spec = library::test::TrackSpec{};
      spec.composer = "Beethoven";
      spec.work = "Symphony No. 5";
      spec.album = r.album;
      spec.movement = std::format("Movement {}", r.movementNumber);
      spec.movementNumber = r.movementNumber;
      spec.movementTotal = 3;
      spec.trackNumber = r.trackNumber;
      ids.push_back(env.lib.addTrack(spec));
    }

    env.setupFiltered(ids);

    auto proj = env.createProjection(ViewId{1});
    auto const sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{.groupBy = TrackGroupKey::Work,
                                               .sortBy = {
                                                 TrackSortTerm{.field = TrackSortField::Composer, .ascending = true},
                                                 TrackSortTerm{.field = TrackSortField::Work, .ascending = true},
                                                 TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                                                 TrackSortTerm{.field = TrackSortField::Movement, .ascending = true},
                                                 TrackSortTerm{.field = TrackSortField::TrackNumber, .ascending = true},
                                               }});

    REQUIRE(proj.size() == 6);

    auto const& dictionary = env.lib.library().dictionary();
    auto transaction = env.lib.library().readTransaction();
    auto reader = env.lib.library().tracks().reader(transaction);

    auto orderedAlbums = std::vector<std::string>{};
    auto orderedMovements = std::vector<std::uint16_t>{};

    for (std::size_t i = 0; i < proj.size(); ++i)
    {
      auto const optView = reader.get(proj.trackIdAt(i), TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      orderedAlbums.emplace_back(dictionary.get(optView->metadata().albumId()));
      orderedMovements.push_back(optView->classical().movementNumber());
    }

    // Karajan (alphabetically first) movements 1,2,3, then Kleiber movements 1,2,3.
    CHECK(orderedAlbums ==
          std::vector<std::string>{
            "Karajan 1963", "Karajan 1963", "Karajan 1963", "Kleiber 1974", "Kleiber 1974", "Kleiber 1974"});
    CHECK(orderedMovements == std::vector<std::uint16_t>{1, 2, 3, 1, 2, 3});
  }

  TEST_CASE("TrackListProjection - sort 10 identical tracks preserves stability", "[runtime][unit][projection]")
  {
    auto env = TestEnv{};
    auto ids = std::vector<TrackId>{};
    ids.reserve(10);

    for (std::int32_t i = 0; i < 10; ++i)
    {
      ids.push_back(env.lib.addTrack(library::test::makeTrackSpec("Same", 2020)));
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

  TEST_CASE("TrackListProjection - sort reversal 10 tracks avoids IO", "[runtime][unit][projection]")
  {
    auto env = TestEnv{};
    auto ids = std::vector<TrackId>{};
    ids.reserve(10);

    for (std::int32_t y = 2019; y >= 2010; --y)
    {
      ids.push_back(
        env.lib.addTrack(library::test::makeTrackSpec(std::format("{}", y), static_cast<std::uint16_t>(y))));
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
        auto transaction = env.lib.library().readTransaction();
        auto reader = env.lib.library().tracks().reader(transaction);
        auto optA = reader.get(proj.trackIdAt(i), TrackStore::Reader::LoadMode::Hot);
        auto optB = reader.get(proj.trackIdAt(i + 1), TrackStore::Reader::LoadMode::Hot);
        REQUIRE(optA);
        REQUIRE(optB);
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

  TEST_CASE("TrackListProjection - switch year to title sort on 15 tracks", "[runtime][unit][projection]")
  {
    auto env = TestEnv{};
    auto ids = std::vector<TrackId>{};
    ids.reserve(15);

    for (std::int32_t i = 0; i < 15; ++i)
    {
      auto title = std::string(1, static_cast<char>('A' + ((i * 7) % 15)));
      ids.push_back(
        env.lib.addTrack(library::test::makeTrackSpec(title, static_cast<std::uint16_t>(2000 + ((i * 3) % 20)))));
    }

    env.setupFiltered(ids);

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{
      .groupBy = TrackGroupKey::None, .sortBy = {TrackSortTerm{.field = TrackSortField::Year, .ascending = true}}});
    REQUIRE(proj.size() == 15);

    for (std::size_t i = 0; i < 14; ++i)
    {
      auto transaction = env.lib.library().readTransaction();
      auto reader = env.lib.library().tracks().reader(transaction);
      auto optA = reader.get(proj.trackIdAt(i), TrackStore::Reader::LoadMode::Hot);
      auto optB = reader.get(proj.trackIdAt(i + 1), TrackStore::Reader::LoadMode::Hot);
      REQUIRE(optA);
      REQUIRE(optB);
      CHECK(optA->metadata().year() <= optB->metadata().year());
    }

    proj.setPresentation(TrackPresentationSpec{
      .groupBy = TrackGroupKey::None, .sortBy = {TrackSortTerm{.field = TrackSortField::Title, .ascending = true}}});
    REQUIRE(proj.size() == 15);

    for (std::size_t i = 0; i < 14; ++i)
    {
      auto transaction = env.lib.library().readTransaction();
      auto reader = env.lib.library().tracks().reader(transaction);
      auto optA = reader.get(proj.trackIdAt(i), TrackStore::Reader::LoadMode::Hot);
      auto optB = reader.get(proj.trackIdAt(i + 1), TrackStore::Reader::LoadMode::Hot);
      REQUIRE(optA);
      REQUIRE(optB);
      CHECK(std::string{optA->metadata().title()} <= std::string{optB->metadata().title()});
    }
  }

  TEST_CASE("TrackListProjection - sort by duration", "[runtime][unit][projection]")
  {
    auto env = TestEnv{};

    auto id1 = env.lib.addTrack(library::test::TrackSpec{.title = "A", .duration = std::chrono::seconds{5}});
    auto id2 = env.lib.addTrack(library::test::TrackSpec{.title = "B", .duration = std::chrono::seconds{3}});
    env.setupFiltered({{id1, id2}});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{.groupBy = TrackGroupKey::None,
                                               .sortBy = {
                                                 TrackSortTerm{.field = TrackSortField::Duration, .ascending = true},
                                               }});

    REQUIRE(proj.size() == 2);
    CHECK(proj.trackIdAt(0) == id2);
    CHECK(proj.trackIdAt(1) == id1);
  }
} // namespace ao::rt::test
