// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackStoreTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWrite.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
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

    void seedTrackRows(std::filesystem::path const& path,
                       bool includeFirstHot,
                       bool includeFirstCold,
                       bool includeSecond)
    {
      // The sweep needs a real metadata header, or it rejects the library before
      // it ever reaches a Track record.
      initializeLibraryStorage(path);

      auto const emptyPayload = std::vector<std::byte>{};
      auto const firstHot = includeFirstHot ? makeHotData() : emptyPayload;
      auto const firstCold =
        includeFirstCold ? makeColdData(TrackColdHeader{.duration = std::chrono::minutes{1}}) : emptyPayload;

      if (includeFirstHot || includeFirstCold)
      {
        seedRawTrackRow(path, 1, firstHot, firstCold);
      }

      if (includeSecond)
      {
        seedRawTrackRow(path, 2, makeHotData(), makeColdData(TrackColdHeader{.duration = std::chrono::minutes{2}}));
      }
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

  TEST_CASE("TrackStore - prepared update replaces the hot side alone", "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    auto const id = addCommittedTrack(
      fixture.library, TrackSpec{.title = "Before", .artist = "First Artist", .duration = std::chrono::minutes{3}});

    {
      auto transaction = writeTransaction(fixture.library);
      auto const replacement = TrackSpec{.title = "After", .artist = "Second Artist"};
      auto builder = makeBuilder(replacement);
      auto prepared = builder.prepareHot(transaction);
      REQUIRE(prepared);
      auto writer = fixture.store.writer(transaction);
      REQUIRE(updatePreparedHotTrackRecord(writer, id, *prepared));
      REQUIRE(transaction.commit());
    }

    auto rtxn = fixture.library.readTransaction();
    auto optView = fixture.store.reader(rtxn).get(id);
    REQUIRE(optView);
    CHECK(optView->metadata().title() == "After");
    CHECK(dictionaryText(fixture.library, optView->metadata().artistId()) == "Second Artist");
    // The cold side is untouched, so its duration survives a hot-only update.
    CHECK(optView->property().duration() == std::chrono::minutes{3});
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
      auto prepared = builder.prepareCold(transaction, fixture.library.resources());
      REQUIRE(prepared);
      auto writer = fixture.store.writer(transaction);
      REQUIRE(updatePreparedColdTrackRecord(writer, id, *prepared));
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
      auto prepared = builder.prepare(transaction, fixture.library.resources());
      REQUIRE(prepared);
      auto writer = fixture.store.writer(transaction);
      REQUIRE(updatePreparedTrackRecord(writer, targetId, prepared->first, prepared->second));
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
    REQUIRE(fixture.store.writer(wtxn).remove(id));
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
    auto const id = addCommittedTrack(
      fixture.library, TrackSpec{.artist = "Iterated Artist", .album = "Iterated Album", .trackNumber = 5});

    auto rtxn = fixture.library.readTransaction();
    auto reader = fixture.store.reader(rtxn);
    auto it = reader.begin(TrackStore::Reader::LoadMode::Hot);
    REQUIRE(it != reader.end(TrackStore::Reader::LoadMode::Hot));
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
    auto it = reader.begin(TrackStore::Reader::LoadMode::Cold);
    REQUIRE(it != reader.end(TrackStore::Reader::LoadMode::Cold));
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
    auto it = reader.begin(TrackStore::Reader::LoadMode::Both);
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

    for (auto const& [id, view] : reader.both())
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
    auto rtxn = fixture.library.readTransaction();
    auto reader = fixture.store.reader(rtxn);

    CHECK(reader.begin(TrackStore::Reader::LoadMode::Cold) == reader.end(TrackStore::Reader::LoadMode::Cold));
  }

  TEST_CASE("TrackStore - iterators from different load modes are distinct", "[library][unit][track-store][raw-layout]")
  {
    auto fixture = TrackStoreFixture{};
    addCommittedTrack(fixture.library, TrackSpec{});

    auto rtxn = fixture.library.readTransaction();
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

  TEST_CASE("MusicLibrary - open rejects a missing cold Track record", "[library][regression][track-store][raw-layout]")
  {
    auto const temp = ao::test::TempDir{};
    seedTrackRows(temp.path(), true, false, false);
    requireCorruptOpen(temp.path());
  }

  TEST_CASE("MusicLibrary - open rejects either kind of orphan Track record",
            "[library][regression][track-store][raw-layout]")
  {
    auto const temp = ao::test::TempDir{};

    SECTION("hot row with missing cold side")
    {
      seedTrackRows(temp.path(), true, false, false);
      requireCorruptOpen(temp.path());
    }

    SECTION("cold row with missing hot side")
    {
      seedTrackRows(temp.path(), false, true, false);
      requireCorruptOpen(temp.path());
    }
  }

  TEST_CASE("MusicLibrary - open rejects a physical orphan instead of normalizing it",
            "[library][regression][track-store][raw-layout]")
  {
    auto const temp = ao::test::TempDir{};
    bool includeHot = false;
    bool includeCold = false;

    SECTION("hot orphan")
    {
      includeHot = true;
    }

    SECTION("cold orphan")
    {
      includeCold = true;
    }

    seedTrackRows(temp.path(), includeHot, includeCold, false);
    requireCorruptOpen(temp.path());
  }

  TEST_CASE("MusicLibrary - open rejects an orphan before exposing later valid Track pairs",
            "[library][regression][track-store][raw-layout]")
  {
    auto const temp = ao::test::TempDir{};
    bool includeFirstHot = true;
    bool includeFirstCold = true;

    SECTION("hot row lacks its cold pair")
    {
      includeFirstCold = false;
    }

    SECTION("cold row lacks its hot pair")
    {
      includeFirstHot = false;
    }

    seedTrackRows(temp.path(), includeFirstHot, includeFirstCold, true);
    requireCorruptOpen(temp.path());
  }
} // namespace ao::library::test
