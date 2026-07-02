// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/projection/TrackListProjectionTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/projection/ProjectionTypes.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("TrackListProjection - group sections for Artist grouping", "[runtime][unit][projection]")
  {
    auto env = TestEnv{};

    auto id1 =
      env.lib.addTrack(library::test::TrackSpec{.title = "T1", .artist = "Zeppelin", .album = "IV", .trackNumber = 1});
    auto id2 =
      env.lib.addTrack(library::test::TrackSpec{.title = "T2", .artist = "Zeppelin", .album = "IV", .trackNumber = 2});
    auto id3 =
      env.lib.addTrack(library::test::TrackSpec{.title = "T3", .artist = "Abba", .album = "Gold", .trackNumber = 1});
    auto id4 =
      env.lib.addTrack(library::test::TrackSpec{.title = "T4", .artist = "Coldplay", .album = "X", .trackNumber = 1});
    auto id5 =
      env.lib.addTrack(library::test::TrackSpec{.title = "T5", .artist = "Coldplay", .album = "Y", .trackNumber = 1});
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
    CHECK(s0.primaryText == "Abba");
    CHECK(s0.rows.start == 0);
    CHECK(s0.rows.count == 1);

    auto s1 = proj.groupAt(1);
    CHECK(s1.primaryText == "Coldplay");
    CHECK(s1.rows.start == 1);
    CHECK(s1.rows.count == 2);

    auto s2 = proj.groupAt(2);
    CHECK(s2.primaryText == "Zeppelin");
    CHECK(s2.rows.start == 3);
    CHECK(s2.rows.count == 2);

    CHECK(proj.groupIndexAt(0) == 0);
    CHECK(proj.groupIndexAt(1) == 1);
    CHECK(proj.groupIndexAt(2) == 1);
    CHECK(proj.groupIndexAt(3) == 2);
    CHECK(proj.groupIndexAt(4) == 2);
  }

  TEST_CASE("TrackListProjection - large artist groups keep compound sort order", "[runtime][unit][projection]")
  {
    auto env = TestEnv{};

    auto ids = std::vector<TrackId>{};
    auto const add =
      [&](std::string title, std::string artist, std::string album, std::uint16_t disc, std::uint16_t track) -> TrackId
    {
      auto const id = env.lib.addTrack(library::test::TrackSpec{.title = std::move(title),
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

    for (std::size_t idx = 0; idx < expectedOrder.size(); ++idx)
    {
      CHECK(proj.trackIdAt(idx) == expectedOrder[idx]);
      CHECK(proj.indexOf(expectedOrder[idx]) == idx);
    }

    struct ExpectedGroup final
    {
      std::string_view primaryText;
      std::size_t start;
      std::size_t count;
    };

    auto const expectedGroups = std::array{
      ExpectedGroup{.primaryText = "Abba", .start = 0, .count = 5},
      ExpectedGroup{.primaryText = "Coldplay", .start = 5, .count = 5},
      ExpectedGroup{.primaryText = "Daft Punk", .start = 10, .count = 4},
      ExpectedGroup{.primaryText = "Radiohead", .start = 14, .count = 5},
      ExpectedGroup{.primaryText = "Zeppelin", .start = 19, .count = 5},
    };

    REQUIRE(proj.groupCount() == expectedGroups.size());

    for (std::size_t groupIndex = 0; groupIndex < expectedGroups.size(); ++groupIndex)
    {
      auto const group = proj.groupAt(groupIndex);
      CHECK(group.primaryText == expectedGroups.at(groupIndex).primaryText);
      CHECK(group.rows.start == expectedGroups.at(groupIndex).start);
      CHECK(group.rows.count == expectedGroups.at(groupIndex).count);

      for (std::size_t row = group.rows.start; row < group.rows.start + group.rows.count; ++row)
      {
        CHECK(proj.groupIndexAt(row) == groupIndex);
      }
    }

    CHECK_FALSE(proj.groupIndexAt(expectedOrder.size()).has_value());
  }

  TEST_CASE("TrackListProjection - group sections empty for None grouping", "[runtime][unit][projection]")
  {
    auto env = TestEnv{};
    auto id1 = env.lib.addTrack(library::test::makeTrackSpec("T1", 2020));
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
    CHECK(s.primaryText.empty());
    CHECK_FALSE(proj.groupIndexAt(0).has_value());
  }

  TEST_CASE("TrackListProjection - empty source has no group sections", "[runtime][unit][projection]")
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

  TEST_CASE("TrackListProjection - group label for unknown artist", "[runtime][unit][projection]")
  {
    auto env = TestEnv{};

    auto id1 = env.lib.addTrack(library::test::TrackSpec{.title = "T1", .artist = "", .album = "A"});
    env.setupFiltered({{id1}});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{.groupBy = TrackGroupKey::Artist,
                                               .sortBy = {
                                                 TrackSortTerm{.field = TrackSortField::Artist, .ascending = true},
                                               }});

    REQUIRE(proj.size() == 1);
    REQUIRE(proj.groupCount() == 1);
    CHECK(proj.groupAt(0).primaryText == "Unknown Artist");
  }

  TEST_CASE("TrackListProjection - group label for unknown year", "[runtime][unit][projection]")
  {
    auto env = TestEnv{};

    auto id1 = env.lib.addTrack(library::test::TrackSpec{.title = "T1", .year = 0});
    env.setupFiltered({{id1}});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{.groupBy = TrackGroupKey::Year,
                                               .sortBy = {
                                                 TrackSortTerm{.field = TrackSortField::Year, .ascending = true},
                                               }});

    REQUIRE(proj.size() == 1);
    REQUIRE(proj.groupCount() == 1);
    CHECK(proj.groupAt(0).primaryText == "Unknown Year");
  }

  TEST_CASE("TrackListProjection - album groups split by album artist", "[runtime][unit][projection]")
  {
    auto env = TestEnv{};

    // Same album title, different album artists
    auto id1 = env.lib.addTrack(
      library::test::TrackSpec{.title = "T1", .artist = "Ari", .album = "Greatest Hits", .albumArtist = "Artist One"});
    auto id2 = env.lib.addTrack(
      library::test::TrackSpec{.title = "T2", .artist = "Ari", .album = "Greatest Hits", .albumArtist = "Artist Two"});
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
    CHECK(proj.groupAt(0).primaryText == "Greatest Hits");
    CHECK(proj.groupAt(0).secondaryText == "Artist One");
    CHECK(proj.groupAt(0).tertiaryText == "2020");
    CHECK(proj.groupAt(1).primaryText == "Greatest Hits");
    CHECK(proj.groupAt(1).secondaryText == "Artist Two");
    CHECK(proj.groupAt(1).tertiaryText == "2020");
  }

  TEST_CASE("TrackListProjection - presentation returns active grouping and redundant fields snapshot",
            "[runtime][unit][projection]")
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

  TEST_CASE("TrackListProjection - group label for album without album artist", "[runtime][unit][projection]")
  {
    auto env = TestEnv{};

    auto id1 = env.lib.addTrack(library::test::TrackSpec{.title = "T1", .album = "Solo Album", .albumArtist = ""});
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
    CHECK(proj.groupAt(0).primaryText == "Solo Album");
    CHECK(proj.groupAt(0).secondaryText == "Unknown Artist");
  }

  TEST_CASE("TrackListProjection - unknown album label", "[runtime][unit][projection]")
  {
    auto env = TestEnv{};

    auto id1 = env.lib.addTrack(library::test::TrackSpec{.title = "T1", .album = "", .albumArtist = ""});
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
    CHECK(proj.groupAt(0).primaryText == "Unknown Album");
    CHECK(proj.groupAt(0).secondaryText == "Unknown Artist");
  }

  TEST_CASE("TrackListProjection - grouping labels genre composer and work fields", "[runtime][unit][projection]")
  {
    auto env = TestEnv{};

    auto id1 =
      env.lib.addTrack(library::test::TrackSpec{.title = "A", .genre = "Pop", .composer = "Mozart", .work = "Opus 1"});
    auto id2 =
      env.lib.addTrack(library::test::TrackSpec{.title = "B", .genre = "Rock", .composer = "Bach", .work = "Opus 2"});
    env.setupFiltered({{id1, id2}});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    SECTION("Group by Genre")
    {
      proj.setPresentation(TrackPresentationSpec{
        .groupBy = TrackGroupKey::Genre, .sortBy = {TrackSortTerm{.field = TrackSortField::Genre, .ascending = true}}});
      REQUIRE(proj.size() == 2);
      REQUIRE(proj.groupCount() == 2);
      CHECK(proj.groupAt(0).primaryText == "Pop");
      CHECK(proj.groupAt(1).primaryText == "Rock");
    }

    SECTION("Group by Composer")
    {
      proj.setPresentation(
        TrackPresentationSpec{.groupBy = TrackGroupKey::Composer,
                              .sortBy = {TrackSortTerm{.field = TrackSortField::Composer, .ascending = true}}});
      REQUIRE(proj.size() == 2);
      REQUIRE(proj.groupCount() == 2);
      CHECK(proj.groupAt(0).primaryText == "Bach");
      CHECK(proj.groupAt(1).primaryText == "Mozart");
    }

    SECTION("Group by Work")
    {
      proj.setPresentation(TrackPresentationSpec{
        .groupBy = TrackGroupKey::Work, .sortBy = {TrackSortTerm{.field = TrackSortField::Work, .ascending = true}}});
      REQUIRE(proj.size() == 2);
      REQUIRE(proj.groupCount() == 2);
      CHECK(proj.groupAt(0).primaryText == "Opus 1");
      CHECK(proj.groupAt(0).secondaryText == "Mozart");
      CHECK(proj.groupAt(1).primaryText == "Opus 2");
      CHECK(proj.groupAt(1).secondaryText == "Bach");
    }
  }
} // namespace ao::rt::test
