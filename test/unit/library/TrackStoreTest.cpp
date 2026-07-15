// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/library/TrackStoreTestSupport.h"
#include <ao/library/TrackLayout.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

namespace ao::library::test
{
  TEST_CASE("TrackStore - create and read", "[library][unit][track-store]")
  {
    auto fixture = TrackStoreFixture{};
    auto hotHeader = TrackHotHeader{.artistId = DictionaryId{7}, .albumId = DictionaryId{9}, .year = 2024};
    auto hotData = makeHotData(hotHeader, "First track");

    auto coldHeader = TrackColdHeader{.duration = std::chrono::minutes{3}, .trackNumber = 2, .trackTotal = 11};
    auto coldData = makeColdData(coldHeader);

    auto wtxn = fixture.library.writeTransaction();
    auto [id, view] = requireCreate(fixture.store.writer(wtxn), hotData, coldData);
    REQUIRE(wtxn.commit());

    CHECK(view.metadata().title() == "First track");
    CHECK(view.metadata().artistId() == DictionaryId{7});
    CHECK(view.property().duration() == std::chrono::minutes{3});

    auto rtxn = fixture.library.readTransaction();
    auto reader = fixture.store.reader(rtxn);
    auto it = reader.begin();
    REQUIRE(it != reader.end());
    auto&& [readId, readView] = *it;
    CHECK(readId == id);
    CHECK(readView.isHotValid());
    CHECK(readView.isColdValid());
    CHECK(readView.metadata().title() == "First track");
    CHECK(readView.metadata().artistId() == DictionaryId{7});
    CHECK(readView.metadata().albumId() == DictionaryId{9});
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
    auto hotHeader = TrackHotHeader{.artistId = DictionaryId{12}, .year = 1999};
    auto hotData = makeHotData(hotHeader, "Lookup track");
    auto coldHeader = TrackColdHeader{.duration = std::chrono::minutes{4}, .trackNumber = 5};
    auto coldData = makeColdData(coldHeader);
    auto id = createCommittedTrack(fixture.store, fixture.library, hotData, coldData);

    auto rtxn = fixture.library.readTransaction();
    auto optFound = fixture.store.reader(rtxn).get(id);
    REQUIRE(optFound);
    CHECK(optFound->isHotValid());
    CHECK(optFound->isColdValid());
    CHECK(optFound->metadata().title() == "Lookup track");
    CHECK(optFound->metadata().artistId() == DictionaryId{12});
    CHECK(optFound->metadata().year() == 1999);
    CHECK(optFound->property().duration() == std::chrono::minutes{4});
    CHECK(optFound->metadata().trackNumber() == 5);
  }

  TEST_CASE("TrackStore - update", "[library][unit][track-store]")
  {
    auto fixture = TrackStoreFixture{};
    auto hotData = makeHotData(TrackHotHeader{.artistId = DictionaryId{1}}, "Before");
    auto coldData = makeColdData(TrackColdHeader{.duration = std::chrono::minutes{3}});
    auto id = createCommittedTrack(fixture.store, fixture.library, hotData, coldData);

    auto hotData2 = makeHotData(TrackHotHeader{.artistId = DictionaryId{2}, .albumId = DictionaryId{3}}, "After");
    auto wtxn = fixture.library.writeTransaction();
    REQUIRE(fixture.store.writer(wtxn).updateHot(id, hotData2));
    REQUIRE(wtxn.commit());

    auto rtxn = fixture.library.readTransaction();
    auto optView = fixture.store.reader(rtxn).get(id);
    REQUIRE(optView);
    CHECK(optView->metadata().title() == "After");
    CHECK(optView->metadata().artistId() == DictionaryId{2});
    CHECK(optView->metadata().albumId() == DictionaryId{3});
    CHECK(optView->property().duration() == std::chrono::minutes{3});
  }

  TEST_CASE("TrackStore - delete", "[library][unit][track-store]")
  {
    auto fixture = TrackStoreFixture{};
    auto hotData = makeHotData(TrackHotHeader{.artistId = DictionaryId{4}}, "Removed");
    auto coldData = makeColdData(TrackColdHeader{.duration = std::chrono::minutes{2}});
    auto id = createCommittedTrack(fixture.store, fixture.library, hotData, coldData);

    auto wtxn = fixture.library.writeTransaction();
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
    auto hotData1 = makeHotData(TrackHotHeader{.artistId = DictionaryId{1}}, "One");
    auto hotData2 = makeHotData(TrackHotHeader{.artistId = DictionaryId{2}}, "Two");
    auto hotData3 = makeHotData(TrackHotHeader{.artistId = DictionaryId{3}}, "Three");
    auto coldData1 = makeColdData(TrackColdHeader{.trackNumber = 1});
    auto coldData2 = makeColdData(TrackColdHeader{.trackNumber = 2});
    auto coldData3 = makeColdData(TrackColdHeader{.trackNumber = 3});

    auto wtxn = fixture.library.writeTransaction();
    auto id1 = requireCreate(fixture.store.writer(wtxn), hotData1, coldData1).first;
    auto id2 = requireCreate(fixture.store.writer(wtxn), hotData2, coldData2).first;
    auto id3 = requireCreate(fixture.store.writer(wtxn), hotData3, coldData3).first;
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

    for (std::int32_t i = 0; i < 3; ++i)
    {
      auto hotData = makeHotData(TrackHotHeader{.artistId = DictionaryId{static_cast<std::uint32_t>(10 + i)}});
      auto coldData = makeColdData(TrackColdHeader{.trackNumber = static_cast<std::uint16_t>(i + 1)});

      auto wtxn2 = fixture.library.writeTransaction();
      ids.push_back(requireCreate(fixture.store.writer(wtxn2), hotData, coldData).first);
      REQUIRE(wtxn2.commit());
    }

    auto rtxn = fixture.library.readTransaction();
    auto reader = fixture.store.reader(rtxn);
    auto collectedIds = std::vector<TrackId>{};
    auto collectedArtistIds = std::vector<DictionaryId>{};
    auto collectedTrackNumbers = std::vector<std::uint16_t>{};

    for (auto it = reader.begin(); it != reader.end(); ++it)
    {
      auto&& [trackId, trackView] = *it;
      collectedIds.push_back(trackId);
      collectedArtistIds.push_back(trackView.metadata().artistId());
      collectedTrackNumbers.push_back(trackView.metadata().trackNumber());
    }

    CHECK(collectedIds == ids);
    CHECK(collectedArtistIds == std::vector<DictionaryId>{DictionaryId{10}, DictionaryId{11}, DictionaryId{12}});
    CHECK(collectedTrackNumbers == std::vector<std::uint16_t>{1, 2, 3});
  }

  TEST_CASE("TrackStore - visitTracks preserves arbitrary request order and duplicates", "[library][unit][track-store]")
  {
    auto fixture = TrackStoreFixture{};
    auto wtxn = fixture.library.writeTransaction();
    auto writer = fixture.store.writer(wtxn);
    auto const id1 = requireCreate(writer, makeHotData({}, "One"), makeColdData()).first;
    auto const id2 = requireCreate(writer, makeHotData({}, "Two"), makeColdData()).first;
    auto const id3 = requireCreate(writer, makeHotData({}, "Three"), makeColdData()).first;
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
    auto wtxn = fixture.library.writeTransaction();
    auto writer = fixture.store.writer(wtxn);
    auto const id1 = requireCreate(writer, makeHotData({}, "One"), makeColdData()).first;
    auto const id2 = requireCreate(writer, makeHotData({}, "Two"), makeColdData()).first;
    auto const id3 = requireCreate(writer, makeHotData({}, "Three"), makeColdData()).first;
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
