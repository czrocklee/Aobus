// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/library/TrackStore.h>

#include "test/unit/library/TrackStoreTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackWrite.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace ao::library::test
{
  namespace
  {
    TrackBuilder makeBuilder(TrackSpec const& spec)
    {
      auto builder = TrackBuilder::makeEmpty();
      applyTrackSpec(builder, spec);
      return builder;
    }

    TrackId addCommittedTrack(MusicLibrary& library, TrackSpec const& spec)
    {
      return createCommittedTrack(library, makeBuilder(spec));
    }

    std::string dictionaryText(MusicLibrary const& library, DictionaryId id)
    {
      return std::string{library.dictionary().getOrDefault(id)};
    }
  } // namespace

  TEST_CASE("TrackStore - create and read", "[library][unit][track-store]")
  {
    auto fixture = TrackStoreFixture{};
    auto const id = addCommittedTrack(fixture.library,
                                      TrackSpec{.title = "First track",
                                                .artist = "First artist",
                                                .album = "First album",
                                                .year = 2024,
                                                .trackNumber = 2,
                                                .trackTotal = 11,
                                                .duration = std::chrono::minutes{3}});

    auto rtxn = fixture.library.readTransaction();
    auto reader = fixture.store.reader(rtxn);
    auto it = reader.begin();
    REQUIRE(it != reader.end());
    auto&& [readId, readView] = *it;
    CHECK(readId == id);
    CHECK(readView.isHotValid());
    CHECK(readView.isColdValid());
    CHECK(readView.metadata().title() == "First track");
    CHECK(dictionaryText(fixture.library, readView.metadata().artistId()) == "First artist");
    CHECK(dictionaryText(fixture.library, readView.metadata().albumId()) == "First album");
    CHECK(readView.metadata().year() == 2024);
    CHECK(readView.property().duration() == std::chrono::minutes{3});
    CHECK(readView.metadata().trackNumber() == 2);
    CHECK(readView.metadata().trackTotal() == 11);
    ++it;
    CHECK(it == reader.end());
  }

  TEST_CASE("TrackStore - read by id", "[library][unit][track-store]")
  {
    auto fixture = TrackStoreFixture{};
    auto const id = addCommittedTrack(fixture.library,
                                      TrackSpec{.title = "Lookup track",
                                                .artist = "Lookup artist",
                                                .year = 1999,
                                                .trackNumber = 5,
                                                .duration = std::chrono::minutes{4}});

    auto rtxn = fixture.library.readTransaction();
    auto optFound = fixture.store.reader(rtxn).get(id);
    REQUIRE(optFound);
    CHECK(optFound->isHotValid());
    CHECK(optFound->isColdValid());
    CHECK(optFound->metadata().title() == "Lookup track");
    CHECK(dictionaryText(fixture.library, optFound->metadata().artistId()) == "Lookup artist");
    CHECK(optFound->metadata().year() == 1999);
    CHECK(optFound->property().duration() == std::chrono::minutes{4});
    CHECK(optFound->metadata().trackNumber() == 5);
  }

  TEST_CASE("TrackStore - update", "[library][unit][track-store]")
  {
    auto fixture = TrackStoreFixture{};
    auto const id = addCommittedTrack(
      fixture.library, TrackSpec{.title = "Before", .artist = "Before artist", .duration = std::chrono::minutes{3}});

    {
      auto wtxn = writeTransaction(fixture.library);
      auto const replacement = TrackSpec{.title = "After", .artist = "After artist", .album = "After album"};
      auto builder = makeBuilder(replacement);
      auto prepared = builder.prepareHot(wtxn);
      REQUIRE(prepared);
      auto writer = fixture.store.writer(wtxn);
      REQUIRE(updatePreparedHotTrackRecord(writer, id, *prepared));
      REQUIRE(wtxn.commit());
    }

    auto rtxn = fixture.library.readTransaction();
    auto optView = fixture.store.reader(rtxn).get(id);
    REQUIRE(optView);
    CHECK(optView->metadata().title() == "After");
    CHECK(dictionaryText(fixture.library, optView->metadata().artistId()) == "After artist");
    CHECK(dictionaryText(fixture.library, optView->metadata().albumId()) == "After album");
    CHECK(optView->property().duration() == std::chrono::minutes{3});
  }

  TEST_CASE("TrackStore - delete", "[library][unit][track-store]")
  {
    auto fixture = TrackStoreFixture{};
    auto const id = addCommittedTrack(
      fixture.library, TrackSpec{.title = "Removed", .artist = "Removed artist", .duration = std::chrono::minutes{2}});

    auto wtxn = writeTransaction(fixture.library);
    REQUIRE(fixture.store.writer(wtxn).remove(id));
    REQUIRE(wtxn.commit());

    auto rtxn = fixture.library.readTransaction();
    auto reader = fixture.store.reader(rtxn);
    CHECK_FALSE(reader.get(id).has_value());
    auto it = reader.begin();
    CHECK(it == reader.end());
  }

  TEST_CASE("TrackStore - create multiple tracks unique IDs", "[library][unit][track-store]")
  {
    auto fixture = TrackStoreFixture{};
    auto wtxn = writeTransaction(fixture.library);
    auto const id1 =
      requireCreate(fixture.library, wtxn, makeBuilder(TrackSpec{.title = "One", .uri = "one.flac", .trackNumber = 1}));
    auto const id2 =
      requireCreate(fixture.library, wtxn, makeBuilder(TrackSpec{.title = "Two", .uri = "two.flac", .trackNumber = 2}));
    auto const id3 = requireCreate(
      fixture.library, wtxn, makeBuilder(TrackSpec{.title = "Three", .uri = "three.flac", .trackNumber = 3}));
    REQUIRE(wtxn.commit());

    CHECK(id1 != id2);
    CHECK(id2 != id3);
    CHECK(id1 != id3);

    auto rtxn = fixture.library.readTransaction();
    auto reader = fixture.store.reader(rtxn);
    auto collectedIds = std::vector<TrackId>{};
    auto collectedTrackNumbers = std::vector<std::uint16_t>{};

    for (auto it = reader.begin(); it != reader.end(); ++it)
    {
      auto&& [trackId, trackView] = *it;
      collectedIds.push_back(trackId);
      collectedTrackNumbers.push_back(trackView.metadata().trackNumber());
    }

    CHECK(collectedIds == std::vector<TrackId>{id1, id2, id3});
    CHECK(collectedTrackNumbers == std::vector<std::uint16_t>{1, 2, 3});
  }

  TEST_CASE("TrackStore - unified TrackView iteration", "[library][unit][track-store]")
  {
    auto fixture = TrackStoreFixture{};
    auto ids = std::vector<TrackId>{};
    auto expectedArtists = std::vector<std::string>{};

    for (std::int32_t i = 0; i < 3; ++i)
    {
      auto const artist = std::format("Artist {}", 10 + i);
      expectedArtists.push_back(artist);
      ids.push_back(addCommittedTrack(
        fixture.library,
        TrackSpec{
          .artist = artist, .uri = std::format("track_{}.flac", i), .trackNumber = static_cast<std::uint16_t>(i + 1)}));
    }

    auto rtxn = fixture.library.readTransaction();
    auto reader = fixture.store.reader(rtxn);
    auto collectedIds = std::vector<TrackId>{};
    auto collectedArtists = std::vector<std::string>{};
    auto collectedTrackNumbers = std::vector<std::uint16_t>{};

    for (auto it = reader.begin(); it != reader.end(); ++it)
    {
      auto&& [trackId, trackView] = *it;
      collectedIds.push_back(trackId);
      collectedArtists.push_back(dictionaryText(fixture.library, trackView.metadata().artistId()));
      collectedTrackNumbers.push_back(trackView.metadata().trackNumber());
    }

    CHECK(collectedIds == ids);
    CHECK(collectedArtists == expectedArtists);
    CHECK(collectedTrackNumbers == std::vector<std::uint16_t>{1, 2, 3});
  }

  TEST_CASE("TrackStore - visitTracks preserves arbitrary request order and duplicates", "[library][unit][track-store]")
  {
    auto fixture = TrackStoreFixture{};
    auto wtxn = writeTransaction(fixture.library);
    auto const id1 = requireCreate(fixture.library, wtxn, makeBuilder(TrackSpec{.title = "One", .uri = "one.flac"}));
    auto const id2 = requireCreate(fixture.library, wtxn, makeBuilder(TrackSpec{.title = "Two", .uri = "two.flac"}));
    auto const id3 =
      requireCreate(fixture.library, wtxn, makeBuilder(TrackSpec{.title = "Three", .uri = "three.flac"}));
    REQUIRE(wtxn.commit());

    auto const missingId = TrackId{id3.raw() + 1};
    auto const requested = std::vector{id3, missingId, id1, id3, id2};
    auto visitedIds = std::vector<TrackId>{};
    auto titles = std::vector<std::string_view>{};
    auto rtxn = fixture.library.readTransaction();
    auto const reader = fixture.store.reader(rtxn);

    auto visitTrack = [&](TrackId id, TrackView const& view)
    {
      visitedIds.push_back(id);
      titles.push_back(view.metadata().title());
    };
    reader.visitTracks(requested, TrackStore::Reader::LoadMode::Hot, visitTrack);

    CHECK(visitedIds == std::vector<TrackId>{id3, id1, id3, id2});
    CHECK(titles == std::vector<std::string_view>{"Three", "One", "Three", "Two"});
  }

  TEST_CASE("TrackStore - visitTracks skips missing IDs in ascending dense requests", "[library][unit][track-store]")
  {
    auto fixture = TrackStoreFixture{};
    auto wtxn = writeTransaction(fixture.library);
    auto const id1 = requireCreate(fixture.library, wtxn, makeBuilder(TrackSpec{.title = "One", .uri = "one.flac"}));
    auto const id2 = requireCreate(fixture.library, wtxn, makeBuilder(TrackSpec{.title = "Two", .uri = "two.flac"}));
    auto const id3 =
      requireCreate(fixture.library, wtxn, makeBuilder(TrackSpec{.title = "Three", .uri = "three.flac"}));
    REQUIRE(wtxn.commit());

    auto const missingId = TrackId{id3.raw() + 1};
    auto const requested = std::vector{id1, id2, id3, missingId};
    auto visitedIds = std::vector<TrackId>{};
    auto rtxn = fixture.library.readTransaction();
    auto const reader = fixture.store.reader(rtxn);

    auto visitTrack = [&](TrackId id, TrackView const& view)
    {
      REQUIRE(view.isHotValid());
      REQUIRE(view.isColdValid());
      visitedIds.push_back(id);
    };
    reader.visitTracks(requested, TrackStore::Reader::LoadMode::Both, visitTrack);

    CHECK(visitedIds == std::vector<TrackId>{id1, id2, id3});
  }
} // namespace ao::library::test
