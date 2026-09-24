// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/library/TrackWrite.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackStoreTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <format>
#include <ranges>
#include <string>
#include <vector>

namespace ao::library::test
{
  namespace
  {
    template<typename Reader>
    concept HasModeBegin = requires(Reader const& reader) { reader.begin(TrackStore::Reader::LoadMode::Hot); };

    template<typename Reader>
    concept HasModeEnd = requires(Reader const& reader) { reader.end(TrackStore::Reader::LoadMode::Hot); };

    template<typename Reader>
    concept HasBothProjection = requires(Reader const& reader) { reader.both(); };

    template<typename Reader>
    concept HasModeEntryCount =
      requires(Reader const& reader) { reader.entryCount(TrackStore::Reader::LoadMode::Hot); };

    static_assert(std::ranges::input_range<TrackStore::Reader const>);
    static_assert(!HasModeBegin<TrackStore::Reader>);
    static_assert(!HasModeEnd<TrackStore::Reader>);
    static_assert(!HasBothProjection<TrackStore::Reader>);
    static_assert(!HasModeEntryCount<TrackStore::Reader>);

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

  TEST_CASE("TrackStore - stores hot and cold record sides", "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto const id = addCommittedTrack(
      fixture.library,
      TrackSpec{.title = "Stored", .trackNumber = 1, .trackTotal = 10, .duration = std::chrono::minutes{3}});

    auto rtxn = fixture.library.readTransaction();
    auto optView = fixture.store.reader(rtxn).get(id);
    REQUIRE(optView);
    CHECK(optView->metadata().title() == "Stored");
    CHECK(optView->property().duration() == std::chrono::minutes{3});
    CHECK(optView->metadata().trackNumber() == 1);
    CHECK(optView->metadata().trackTotal() == 10);
  }

  TEST_CASE("TrackStore - prepared update replaces the cold side alone", "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto const id = addCommittedTrack(
      fixture.library, TrackSpec{.title = "Kept", .trackNumber = 1, .duration = std::chrono::minutes{3}});

    {
      auto transaction = writeTransaction(fixture.library);
      auto const replacement = TrackSpec{.title = "Kept", .trackNumber = 2, .duration = std::chrono::seconds{200}};
      auto builder = makeBuilder(replacement);
      auto preparedRes = physicalPrepareColdTrack(builder, transaction, fixture.library.resources());
      REQUIRE(preparedRes);
      auto writer = physicalWriter(fixture.store, transaction);
      REQUIRE(updatePreparedColdTrackRecord(writer, id, *preparedRes));
      REQUIRE(transaction.commit());
    }

    auto rtxn = fixture.library.readTransaction();
    auto optView = fixture.store.reader(rtxn).get(id);
    REQUIRE(optView);
    CHECK(optView->metadata().title() == "Kept");
    CHECK(optView->property().duration() == std::chrono::seconds{200});
    CHECK(optView->metadata().trackNumber() == 2);
  }

  TEST_CASE("TrackStore - prepared paired update replaces both sides", "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto const targetId = addCommittedTrack(fixture.library, TrackSpec{.title = "Target"});

    {
      auto transaction = writeTransaction(fixture.library);
      auto const replacement =
        TrackSpec{.title = "Replaced", .artist = "New Artist", .duration = std::chrono::minutes{7}};
      auto builder = makeBuilder(replacement);
      auto preparedRes = physicalPrepareTrack(builder, transaction, fixture.library.resources());
      REQUIRE(preparedRes);
      auto writer = physicalWriter(fixture.store, transaction);
      REQUIRE(updatePreparedTrackRecord(writer, targetId, preparedRes->first, preparedRes->second));
      REQUIRE(transaction.commit());
    }

    auto rtxn = fixture.library.readTransaction();
    auto const optView = fixture.store.reader(rtxn).get(targetId);
    REQUIRE(optView);
    CHECK(optView->metadata().title() == "Replaced");
    CHECK(dictionaryText(fixture.library, optView->metadata().artistId()) == "New Artist");
    CHECK(optView->property().duration() == std::chrono::minutes{7});
  }

  TEST_CASE("TrackStore - remove deletes hot and cold records", "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto const id = addCommittedTrack(fixture.library, TrackSpec{});

    auto wtxn = writeTransaction(fixture.library);
    REQUIRE(physicalWriter(fixture.store, wtxn).tryRemove(id));
    REQUIRE(wtxn.commit());

    auto rtxn = fixture.library.readTransaction();
    auto reader = fixture.store.reader(rtxn);
    CHECK_FALSE(reader.get(id, TrackStore::Reader::LoadMode::Hot).has_value());
    CHECK_FALSE(reader.get(id, TrackStore::Reader::LoadMode::Cold).has_value());
  }

  TEST_CASE("TrackStore - writer get supports load modes", "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto const id = addCommittedTrack(fixture.library, TrackSpec{.duration = std::chrono::minutes{4}});

    auto wtxn = writeTransaction(fixture.library);
    auto writer = physicalWriter(fixture.store, wtxn);
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
    auto const id = addCommittedTrack(
      fixture.library, TrackSpec{.artist = "Iterated Artist", .album = "Iterated Album", .trackNumber = 5});

    auto rtxn = fixture.library.readTransaction();
    auto reader = fixture.store.reader(rtxn);
    auto rows = reader.hot();
    auto it = rows.begin();
    REQUIRE(it != rows.end());
    auto&& [trackId, trackView] = *it;
    CHECK(trackId == id);
    CHECK(trackView.isHotValid());
    CHECK_FALSE(trackView.isColdValid());
  }

  TEST_CASE("TrackStore - cold load mode iteration omits hot data", "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto const id =
      addCommittedTrack(fixture.library, TrackSpec{.trackNumber = 3, .duration = std::chrono::minutes{4}});

    auto rtxn = fixture.library.readTransaction();
    auto reader = fixture.store.reader(rtxn);
    auto rows = reader.cold();
    auto it = rows.begin();
    REQUIRE(it != rows.end());
    auto&& [trackId, trackView] = *it;
    CHECK(trackId == id);
    CHECK_FALSE(trackView.isHotValid());
    CHECK(trackView.isColdValid());
  }

  TEST_CASE("TrackStore - both load mode iteration returns hot and cold data",
            "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto const id = addCommittedTrack(
      fixture.library, TrackSpec{.artist = "Paired Artist", .trackNumber = 0, .duration = std::chrono::minutes{5}});

    auto rtxn = fixture.library.readTransaction();
    auto reader = fixture.store.reader(rtxn);
    auto it = reader.begin();
    REQUIRE(it != reader.end());
    auto&& [trackId, trackView] = *it;
    CHECK(trackId == id);
    CHECK(trackView.isHotValid());
    CHECK(trackView.isColdValid());
    CHECK(trackView.property().duration() == std::chrono::minutes{5});
    CHECK(trackView.metadata().trackNumber() == 0);
  }

  TEST_CASE("TrackStore - both load mode advances paired records in lockstep",
            "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto const ids = std::vector{
      addCommittedTrack(fixture.library, TrackSpec{.title = "First", .uri = "first.flac"}),
      addCommittedTrack(fixture.library, TrackSpec{.title = "Second", .uri = "second.flac"}),
      addCommittedTrack(fixture.library, TrackSpec{.title = "Third", .uri = "third.flac"}),
    };

    auto transaction = fixture.library.readTransaction();
    auto reader = fixture.store.reader(transaction);
    auto actualIds = std::vector<TrackId>{};

    for (auto const& [id, view] : reader)
    {
      actualIds.push_back(id);
      CHECK(view.isHotValid());
      CHECK(view.isColdValid());
    }

    CHECK(actualIds == ids);
  }

  TEST_CASE("TrackStore - hot load mode get by id omits cold data", "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto const id = addCommittedTrack(fixture.library, TrackSpec{.artist = "Hot Only Artist"});

    auto rtxn = fixture.library.readTransaction();
    auto optView = fixture.store.reader(rtxn).get(id, TrackStore::Reader::LoadMode::Hot);
    REQUIRE(optView);
    CHECK(optView->isHotValid());
    CHECK_FALSE(optView->isColdValid());
    CHECK(dictionaryText(fixture.library, optView->metadata().artistId()) == "Hot Only Artist");
  }

  TEST_CASE("TrackStore - cold load mode get by id omits hot data", "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto const id =
      addCommittedTrack(fixture.library, TrackSpec{.artist = "Cold Only Artist", .duration = std::chrono::minutes{6}});

    auto rtxn = fixture.library.readTransaction();
    auto optView = fixture.store.reader(rtxn).get(id, TrackStore::Reader::LoadMode::Cold);
    REQUIRE(optView);
    CHECK_FALSE(optView->isHotValid());
    CHECK(optView->isColdValid());
    CHECK(optView->property().duration() == std::chrono::minutes{6});
  }

  TEST_CASE("TrackStore - cold load mode iterates multiple records", "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto ids = std::vector<TrackId>{};

    for (std::int32_t i = 0; i < 5; ++i)
    {
      ids.push_back(addCommittedTrack(fixture.library,
                                      TrackSpec{.artist = std::format("Artist {}", i),
                                                .uri = std::format("track_{}.flac", i),
                                                .trackNumber = static_cast<std::uint16_t>(i + 1),
                                                .duration = std::chrono::seconds{180 + (i * 10)}}));
    }

    auto rtxn = fixture.library.readTransaction();
    auto reader = fixture.store.reader(rtxn);
    auto rows = reader.cold();
    auto it = rows.begin();
    auto endIt = rows.end();
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
    auto rtxn = fixture.library.readTransaction();
    auto reader = fixture.store.reader(rtxn);
    auto rows = reader.cold();

    CHECK(rows.begin() == rows.end());
  }

  TEST_CASE("MusicLibrary - open rejects either kind of orphan Track record",
            "[library][unit][track-store][raw-layout]")
  {
    auto const temp = ao::test::TempDir{};
    initializeLibraryStorage(temp.path());

    SECTION("hot row with missing cold side")
    {
      seedRawTrackRow(temp.path(), 1, "track.flac", makeHotData(), {});
    }

    SECTION("cold row with missing hot side")
    {
      seedRawTrackRow(temp.path(), 1, "track.flac", {}, makeColdData());
    }

    requireCorruptOpen(temp.path(), "Hot and cold Track databases contain different key sets");
  }

  TEST_CASE("MusicLibrary - open rejects an orphan before exposing later valid Track pairs",
            "[library][unit][track-store][raw-layout]")
  {
    auto const temp = ao::test::TempDir{};
    initializeLibraryStorage(temp.path());
    seedRawTrackRow(temp.path(),
                    2,
                    "second.flac",
                    makeHotData(),
                    makeColdData(TrackColdHeader{.duration = std::chrono::minutes{2}}, "second.flac"));

    {
      auto const validRes = openTestMusicLibrary(temp.path(), temp.path());
      REQUIRE(validRes);
    }

    SECTION("hot row lacks its cold pair")
    {
      seedRawTrackRow(temp.path(), 1, "first.flac", makeHotData(), {});
      requireCorruptOpen(temp.path(), "Hot and cold Track keys do not form matching nonzero pairs: 1 and 2");
    }

    SECTION("cold row lacks its hot pair")
    {
      seedRawTrackRow(temp.path(),
                      1,
                      "first.flac",
                      {},
                      makeColdData(TrackColdHeader{.duration = std::chrono::minutes{1}}, "first.flac"));
      requireCorruptOpen(temp.path(), "Hot and cold Track keys do not form matching nonzero pairs: 2 and 1");
    }
  }
} // namespace ao::library::test
