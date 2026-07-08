// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/library/TrackStoreTestSupport.h"
#include "test/unit/lmdb/TestUtils.h"
#include <ao/library/TrackLayout.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ao::library::test
{
  TEST_CASE("TrackStore - createHotCold stores raw hot and cold records", "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto hotData = makeHotData();
    auto coldData =
      makeColdData(TrackColdHeader{.duration = std::chrono::minutes{3}, .trackNumber = 1, .trackTotal = 10});

    auto wtxn = beginWriteTransaction(fixture.env);
    auto id = requireCreate(fixture.store.writer(wtxn), hotData, coldData).first;
    REQUIRE(wtxn.commit());

    auto rtxn = beginReadTransaction(fixture.env);
    auto optView = fixture.store.reader(rtxn).get(id);
    REQUIRE(optView);
    CHECK(optView->property().duration() == std::chrono::minutes{3});
    CHECK(optView->metadata().trackNumber() == 1);
    CHECK(optView->metadata().trackTotal() == 10);
  }

  TEST_CASE("TrackStore - rejects raw records that violate the alignment contract",
            "[library][regression][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto hotData = makeHotData();
    auto coldData = makeColdData();
    auto hotBacking = std::vector<std::byte>(hotData.size() + 1, std::byte{0});
    std::ranges::copy(hotData, hotBacking.begin() + 1);

    auto wtxn = beginWriteTransaction(fixture.env);
    auto writer = fixture.store.writer(wtxn);

    SECTION("createHotCold rejects unaligned input spans")
    {
      auto const hotSpan = std::span<std::byte const>{hotBacking.data() + 1, hotData.size()};
      auto const result = writer.createHotCold(hotSpan, coldData);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("updateCold rejects sizes that are not 4-byte aligned")
    {
      auto const id = requireCreate(writer, hotData, coldData).first;
      auto const result = writer.updateCold(id, coldData.size() + 1);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }
  }

  TEST_CASE("TrackStore - updateHot and updateCold replace separate raw records",
            "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto hotData = makeHotData();
    auto coldData = makeColdData(TrackColdHeader{.duration = std::chrono::minutes{3}});
    auto id = createCommittedTrack(fixture.store, fixture.env, hotData, coldData);

    auto hotData2 = makeHotData(TrackHotHeader{.artistId = DictionaryId{99}});
    auto wtxn1 = beginWriteTransaction(fixture.env);
    REQUIRE(fixture.store.writer(wtxn1).updateHot(id, hotData2));
    REQUIRE(wtxn1.commit());

    auto coldData2 = makeColdData(TrackColdHeader{.duration = std::chrono::seconds{200}, .trackNumber = 2});
    auto wtxn2 = beginWriteTransaction(fixture.env);
    REQUIRE(fixture.store.writer(wtxn2).updateCold(
      id, coldData2.size(), [&](std::span<std::byte> buf) { std::ranges::copy(coldData2, buf.begin()); }));
    REQUIRE(wtxn2.commit());

    auto rtxn = beginReadTransaction(fixture.env);
    auto optView = fixture.store.reader(rtxn).get(id);
    REQUIRE(optView);
    CHECK(optView->metadata().artistId() == DictionaryId{99});
    CHECK(optView->property().duration() == std::chrono::seconds{200});
    CHECK(optView->metadata().trackNumber() == 2);
  }

  TEST_CASE("TrackStore - remove deletes hot and cold records", "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto id = createCommittedTrack(fixture.store, fixture.env, makeHotData(), makeColdData());

    auto wtxn = beginWriteTransaction(fixture.env);
    REQUIRE(fixture.store.writer(wtxn).remove(id));
    REQUIRE(wtxn.commit());

    auto rtxn = beginReadTransaction(fixture.env);
    CHECK_FALSE(fixture.store.reader(rtxn).get(id).has_value());
  }

  TEST_CASE("TrackStore - writer get supports load modes", "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto id = createCommittedTrack(
      fixture.store, fixture.env, makeHotData(), makeColdData(TrackColdHeader{.duration = std::chrono::minutes{4}}));

    auto wtxn = beginWriteTransaction(fixture.env);
    auto writer = fixture.store.writer(wtxn);
    auto optHot = writer.get(id, TrackStore::Reader::LoadMode::Hot);
    REQUIRE(optHot);
    CHECK(optHot->isHotValid());
    CHECK_FALSE(optHot->isColdValid());

    auto optCold = writer.get(id, TrackStore::Reader::LoadMode::Cold);
    REQUIRE(optCold);
    CHECK_FALSE(optCold->isHotValid());
    CHECK(optCold->isColdValid());
    CHECK(optCold->property().duration() == std::chrono::minutes{4});
    CHECK(optCold->coverArt().count() == 0);
  }

  TEST_CASE("TrackStore - hot load mode iteration omits cold data", "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto id =
      createCommittedTrack(fixture.store,
                           fixture.env,
                           makeHotData(TrackHotHeader{.artistId = DictionaryId{1}, .albumId = DictionaryId{2}}),
                           makeColdData(TrackColdHeader{.duration = std::chrono::minutes{3}, .trackNumber = 5}));

    auto rtxn = beginReadTransaction(fixture.env);
    auto reader = fixture.store.reader(rtxn);
    auto it = reader.begin(TrackStore::Reader::LoadMode::Hot);
    REQUIRE(it != reader.end());
    auto&& [trackId, trackView] = *it;
    CHECK(trackId == id);
    CHECK(trackView.isHotValid());
    CHECK_FALSE(trackView.isColdValid());
  }

  TEST_CASE("TrackStore - cold load mode iteration omits hot data", "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto id =
      createCommittedTrack(fixture.store,
                           fixture.env,
                           makeHotData(),
                           makeColdData(TrackColdHeader{.duration = std::chrono::minutes{4}, .trackNumber = 3}));

    auto rtxn = beginReadTransaction(fixture.env);
    auto reader = fixture.store.reader(rtxn);
    auto it = reader.begin(TrackStore::Reader::LoadMode::Cold);
    REQUIRE(it != reader.end());
    auto&& [trackId, trackView] = *it;
    CHECK(trackId == id);
    CHECK_FALSE(trackView.isHotValid());
    CHECK(trackView.isColdValid());
  }

  TEST_CASE("TrackStore - both load mode iteration returns hot and cold data",
            "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto id = createCommittedTrack(fixture.store,
                                   fixture.env,
                                   makeHotData(TrackHotHeader{.artistId = DictionaryId{1}}),
                                   makeColdData(TrackColdHeader{.duration = std::chrono::minutes{5}}));

    auto rtxn = beginReadTransaction(fixture.env);
    auto reader = fixture.store.reader(rtxn);
    auto it = reader.begin(TrackStore::Reader::LoadMode::Both);
    REQUIRE(it != reader.end());
    auto&& [trackId, trackView] = *it;
    CHECK(trackId == id);
    CHECK(trackView.isHotValid());
    CHECK(trackView.isColdValid());
    CHECK(trackView.property().duration() == std::chrono::minutes{5});
    CHECK(trackView.metadata().trackNumber() == 0);
  }

  TEST_CASE("TrackStore - hot load mode get by id omits cold data", "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto id = createCommittedTrack(
      fixture.store, fixture.env, makeHotData(TrackHotHeader{.artistId = DictionaryId{42}}), makeColdData());

    auto rtxn = beginReadTransaction(fixture.env);
    auto optView = fixture.store.reader(rtxn).get(id, TrackStore::Reader::LoadMode::Hot);
    REQUIRE(optView);
    CHECK(optView->isHotValid());
    CHECK_FALSE(optView->isColdValid());
    CHECK(optView->metadata().artistId() == DictionaryId{42});
  }

  TEST_CASE("TrackStore - cold load mode get by id omits hot data", "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto id = createCommittedTrack(fixture.store,
                                   fixture.env,
                                   makeHotData(TrackHotHeader{.artistId = DictionaryId{99}}),
                                   makeColdData(TrackColdHeader{.duration = std::chrono::minutes{6}}));

    auto rtxn = beginReadTransaction(fixture.env);
    auto optView = fixture.store.reader(rtxn).get(id, TrackStore::Reader::LoadMode::Cold);
    REQUIRE(optView);
    CHECK_FALSE(optView->isHotValid());
    CHECK(optView->isColdValid());
    CHECK(optView->property().duration() == std::chrono::minutes{6});
  }

  TEST_CASE("TrackStore - cold load mode iterates multiple raw records", "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto ids = std::vector<TrackId>{};

    for (std::int32_t i = 0; i < 5; ++i)
    {
      ids.push_back(
        createCommittedTrack(fixture.store,
                             fixture.env,
                             makeHotData(TrackHotHeader{.artistId = DictionaryId{static_cast<std::uint32_t>(i)}}),
                             makeColdData(TrackColdHeader{.duration = std::chrono::seconds{180 + (i * 10)},
                                                          .trackNumber = static_cast<std::uint16_t>(i + 1)})));
    }

    auto rtxn = beginReadTransaction(fixture.env);
    auto reader = fixture.store.reader(rtxn);
    auto it = reader.begin(TrackStore::Reader::LoadMode::Cold);
    auto endIt = reader.end(TrackStore::Reader::LoadMode::Cold);
    auto collectedIds = std::vector<TrackId>{};

    while (it != endIt)
    {
      auto&& [trackId, trackView] = *it;
      collectedIds.push_back(trackId);
      CHECK_FALSE(trackView.isHotValid());
      CHECK(trackView.isColdValid());
      ++it;
    }

    CHECK(collectedIds == ids);
  }

  TEST_CASE("TrackStore - cold load mode empty iteration returns end", "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto rtxn = beginReadTransaction(fixture.env);
    auto reader = fixture.store.reader(rtxn);

    CHECK(reader.begin(TrackStore::Reader::LoadMode::Cold) == reader.end(TrackStore::Reader::LoadMode::Cold));
  }

  TEST_CASE("TrackStore - iterators from different load modes are distinct", "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    createCommittedTrack(fixture.store, fixture.env, makeHotData(), makeColdData());

    auto rtxn = beginReadTransaction(fixture.env);
    auto reader = fixture.store.reader(rtxn);
    auto coldBegin = reader.begin(TrackStore::Reader::LoadMode::Cold);
    auto hotBegin = reader.begin(TrackStore::Reader::LoadMode::Hot);
    auto bothBegin = reader.begin(TrackStore::Reader::LoadMode::Both);

    CHECK(coldBegin != hotBegin);
    CHECK(hotBegin != bothBegin);
    CHECK(coldBegin != bothBegin);
    CHECK(reader.end() != coldBegin);
    CHECK(reader.end() != hotBegin);
    CHECK(reader.end() != bothBegin);
  }

  TEST_CASE("TrackStore - missing cold record makes both-mode read fail",
            "[library][regression][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto wtxn = beginWriteTransaction(fixture.env);
    auto writer = fixture.store.writer(wtxn);
    auto id = requireCreate(writer, createStringData("hot_"), createStringData("cold")).first;

    auto coldDb = openDatabase(wtxn, "tracks_cold");
    REQUIRE(coldDb.writer(wtxn).del(id.raw()));
    REQUIRE(wtxn.commit());

    auto rtxn = beginReadTransaction(fixture.env);
    auto optView = fixture.store.reader(rtxn).get(id, TrackStore::Reader::LoadMode::Both);
    CHECK_FALSE(optView);
  }
} // namespace ao::library::test
