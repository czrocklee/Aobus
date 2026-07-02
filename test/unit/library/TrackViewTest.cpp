// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/library/TestUtils.h"
#include "test/unit/library/TrackViewTestSupport.h"
#include "test/unit/lmdb/TestUtils.h"
#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/library/CoverArt.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::library::test
{
  namespace
  {
#if defined(__GNUC__) && !defined(__clang__)
    static_assert(std::ranges::view<TrackView::CoverArtProxy>);
#endif

    using namespace ao::lmdb::test;
  } // namespace

  // === Metadata Tests ===
  TEST_CASE("TrackView - returns title from hot data", "[library][unit][track]")
  {
    auto const data = makeHotTrackViewData("Test Title");
    auto const view = TrackView{data, std::span<std::byte const>{}};
    CHECK(view.metadata().title() == "Test Title");
  }

  TEST_CASE("TrackView - returns empty title when hot title length is zero", "[library][unit][track]")
  {
    auto const data = makeMinimalHotTrackViewData();
    auto const view = TrackView{data, std::span<std::byte const>{}};
    CHECK(view.metadata().title().empty());
  }

  TEST_CASE("TrackView - returns dictionary IDs from hot data", "[library][unit][track]")
  {
    auto const data = makeMinimalHotTrackViewData();
    auto const view = TrackView{data, std::span<std::byte const>{}};

    CHECK(view.metadata().artistId() == DictionaryId{1});
    CHECK(view.metadata().albumId() == DictionaryId{2});
    CHECK(view.metadata().genreId() == DictionaryId{3});
    CHECK(view.metadata().albumArtistId() == kInvalidDictionaryId);
    CHECK(view.metadata().composerId() == kInvalidDictionaryId);
  }

  TEST_CASE("TrackView - returns year from hot data", "[library][unit][track]")
  {
    auto const data = makeMinimalHotTrackViewData();
    auto const view = TrackView{data, std::span<std::byte const>{}};
    CHECK(view.metadata().year() == 2020);
  }

  TEST_CASE("TrackView - returns track numbering from cold data", "[library][unit][track]")
  {
    auto header = TrackColdHeader{};
    header.trackNumber = 5;
    header.trackTotal = 10;
    header.discNumber = 1;
    header.discTotal = 2;

    auto const data = makeColdTrackViewData(header, {}, "/path/to/file.flac");
    auto const view = makeColdTrackView(data);

    CHECK(view.metadata().trackNumber() == 5);
    CHECK(view.metadata().trackTotal() == 10);
    CHECK(view.metadata().discNumber() == 1);
    CHECK(view.metadata().discTotal() == 2);
  }

  TEST_CASE("TrackView - returns work and movement IDs from cold data", "[library][unit][track]")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata()
      .work("Symphony No. 9 in D minor, Op. 125")
      .movement("II. Molto vivace")
      .movementNumber(2)
      .movementTotal(4);

    auto temp = ao::test::TempDir{};
    auto env = lmdb::test::openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = lmdb::test::beginWriteTransaction(env);
    auto dict = DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};
    auto resources = ResourceStore{lmdb::test::openDatabase(wtxn, "resources")};
    auto coldDataResult = builder.serializeCold(wtxn, dict, resources);
    REQUIRE(coldDataResult);
    auto const& coldData = *coldDataResult;
    auto const view = makeColdTrackView(coldData);

    CHECK(view.metadata().workId().raw() > 0);
    CHECK(view.metadata().movementId().raw() > 0);
    CHECK(dict.get(view.metadata().workId()) == "Symphony No. 9 in D minor, Op. 125");
    CHECK(dict.get(view.metadata().movementId()) == "II. Molto vivace");
    CHECK(view.metadata().movementNumber() == 2);
    CHECK(view.metadata().movementTotal() == 4);
  }

  TEST_CASE("TrackView - returns cover art entries from cold data", "[library][unit][track]")
  {
    auto builder = TrackBuilder::createNew();
    builder.coverArt().add(PictureType::BackCover, ResourceId{42});

    auto temp = ao::test::TempDir{};
    auto env = lmdb::test::openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = lmdb::test::beginWriteTransaction(env);
    auto dict = DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};
    auto resources = ResourceStore{lmdb::test::openDatabase(wtxn, "resources")};
    auto coldDataResult = builder.serializeCold(wtxn, dict, resources);
    REQUIRE(coldDataResult);
    auto const& coldData = *coldDataResult;

    auto const view = makeColdTrackView(coldData);
    REQUIRE(view.coverArt().count() == 1);
    CHECK(view.coverArt().at(0).resourceId == ResourceId{42});
    CHECK(view.coverArt().at(0).type == PictureType::BackCover);
    REQUIRE(view.coverArt().primary());
    CHECK(view.coverArt().primary()->resourceId == ResourceId{42});
    CHECK(view.coverArt().primary()->type == PictureType::BackCover);
  }

  TEST_CASE("TrackView - returns URI from cold data", "[library][unit][track]")
  {
    auto const data = makeColdTrackViewData({}, {}, "/path/to/file.flac");
    auto const view = makeColdTrackView(data);

    CHECK_FALSE(view.coverArt().primary().has_value());
    CHECK(view.property().uri() == "/path/to/file.flac");
  }

  TEST_CASE("TrackView - returns empty URI when cold URI length is zero", "[library][unit][track]")
  {
    auto const data = makeColdTrackViewData({}, {}, "");
    auto const view = makeColdTrackView(data);

    CHECK(view.property().uri().empty());
  }

  // === Property Tests ===
  TEST_CASE("TrackView - returns codec and bit depth from hot data", "[library][unit][track]")
  {
    auto h = TrackHotHeader{};
    h.codec = AudioCodec::Flac;
    h.bitDepth = BitDepth{24};
    h.sampleRate = SampleRate{96000};
    auto const data = serializeHeader(h);
    auto const view = TrackView{data, std::span<std::byte const>{}};

    CHECK(view.property().codec() == AudioCodec::Flac);
    CHECK(view.property().bitDepth() == 24);
    CHECK(view.property().sampleRate() == 96000);
  }

  TEST_CASE("TrackView - returns file size and modification time from cold data", "[library][unit][track]")
  {
    auto const data = makeColdTrackViewData({}, {}, "");
    auto const view = makeColdTrackView(data);
    CHECK(view.isColdValid());
  }

  TEST_CASE("TrackView - returns audio format from cold data", "[library][unit][track]")
  {
    auto header = TrackColdHeader{};
    header.duration = std::chrono::minutes{3};
    header.bitrate = Bitrate{320000};
    header.channels = Channels{2};

    auto const data = makeColdTrackViewData(header, {}, "");
    auto const view = makeColdTrackView(data);

    CHECK(view.property().duration() == std::chrono::minutes{3});
    CHECK(view.property().bitrate() == 320000);
    CHECK(view.property().channels() == 2);
  }
} // namespace ao::library::test
