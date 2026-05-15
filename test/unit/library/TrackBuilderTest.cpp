// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <lmdb.h>
#include <test/unit/lmdb/TestUtils.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace ao::library::test
{
  using namespace ao::lmdb;
  using namespace ao::lmdb::test;

  namespace
  {
    std::pair<std::vector<std::byte>, std::vector<std::byte>> serializeTestTrack(TrackBuilder& builder)
    {
      auto temp = TempDir{};
      auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
      auto wtxn = WriteTransaction{env};
      auto dict = DictionaryStore{lmdb::Database{wtxn, "dict"}, wtxn};
      auto resources = ResourceStore{lmdb::Database{wtxn, "resources"}};
      return builder.serialize(wtxn, dict, resources);
    }
  } // namespace

  TEST_CASE("TrackBuilder - Default Constructor via createNew")
  {
    auto builder = TrackBuilder::createNew();

    CHECK(builder.metadata().title().empty());
    CHECK(builder.metadata().artist().empty());
    CHECK(builder.metadata().album().empty());
    CHECK(builder.metadata().albumArtist().empty());
    CHECK(builder.metadata().composer().empty());
    CHECK(builder.metadata().genre().empty());
    CHECK(builder.property().uri().empty());
    CHECK(builder.property().fileSize() == 0);
    CHECK(builder.property().bitDepth() == 0);
    CHECK(builder.tags().names().empty());
    CHECK(builder.custom().pairs().empty());
  }

  TEST_CASE("TrackBuilder - MetadataBuilder fluent setters")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata()
      .title("Test Title")
      .artist("Test Artist")
      .album("Test Album")
      .albumArtist("Test Album Artist")
      .composer("Test Composer")
      .genre("Rock")
      .year(2024)
      .trackNumber(5)
      .totalTracks(10)
      .discNumber(2)
      .totalDiscs(3)
      .coverArtId(42)
      .rating(4);

    CHECK(builder.metadata().title() == "Test Title");
    CHECK(builder.metadata().artist() == "Test Artist");
    CHECK(builder.metadata().album() == "Test Album");
    CHECK(builder.metadata().albumArtist() == "Test Album Artist");
    CHECK(builder.metadata().composer() == "Test Composer");
    CHECK(builder.metadata().genre() == "Rock");
    CHECK(builder.metadata().year() == 2024);
    CHECK(builder.metadata().trackNumber() == 5);
    CHECK(builder.metadata().totalTracks() == 10);
    CHECK(builder.metadata().discNumber() == 2);
    CHECK(builder.metadata().totalDiscs() == 3);
    CHECK(builder.metadata().coverArtId() == 42);
    CHECK(builder.metadata().rating() == 4);
  }

  TEST_CASE("TrackBuilder - PropertyBuilder fluent setters")
  {
    auto builder = TrackBuilder::createNew();
    builder.property()
      .fileSize(10000000)
      .mtime(1234567890)
      .durationMs(180000)
      .bitrate(320000)
      .sampleRate(44100)
      .codecId(7)
      .channels(2)
      .bitDepth(16)
      .uri("/path/to/track.flac");

    CHECK(builder.property().fileSize() == 10000000);
    CHECK(builder.property().mtime() == 1234567890);
    CHECK(builder.property().durationMs() == 180000);
    CHECK(builder.property().bitrate() == 320000);
    CHECK(builder.property().sampleRate() == 44100);
    CHECK(builder.property().codecId() == 7);
    CHECK(builder.property().channels() == 2);
    CHECK(builder.property().bitDepth() == 16);
    CHECK(builder.property().uri() == "/path/to/track.flac");
  }

  TEST_CASE("TrackBuilder - TagsBuilder add/remove/clear")
  {
    auto builder = TrackBuilder::createNew();

    // Add tags
    builder.tags().add("rock");
    builder.tags().add("jazz");
    builder.tags().add("blues");

    CHECK(builder.tags().names().size() == 3);

    // Remove a tag
    builder.tags().remove("jazz");
    CHECK(builder.tags().names().size() == 2);

    // Check remaining tags
    CHECK(std::ranges::contains(builder.tags().names(), "rock"));
    CHECK(std::ranges::contains(builder.tags().names(), "blues"));

    // Clear
    builder.tags().clear();
    CHECK(builder.tags().names().empty());
  }

  TEST_CASE("TrackBuilder - CustomBuilder add/remove/clear")
  {
    auto builder = TrackBuilder::createNew();

    // Add custom pairs
    builder.custom().add("replaygain_track_gain_db", "-6.5");
    builder.custom().add("isrc", "USSM19999999");

    CHECK(builder.custom().pairs().size() == 2);

    // Remove a pair
    builder.custom().remove("isrc");
    CHECK(builder.custom().pairs().size() == 1);
    CHECK(builder.custom().pairs()[0].first == "replaygain_track_gain_db");
    CHECK(builder.custom().pairs()[0].second == "-6.5");

    // Clear
    builder.custom().clear();
    CHECK(builder.custom().pairs().empty());
  }

  TEST_CASE("TrackBuilder - Chained API")
  {
    auto builder = TrackBuilder::createNew();

    builder.metadata().title("Song").artist("Artist").album("Album");
    builder.property().fileSize(1000).bitDepth(16);
    builder.tags().add("rock").add("jazz");
    builder.custom().add("key", "value");

    CHECK(builder.metadata().title() == "Song");
    CHECK(builder.metadata().artist() == "Artist");
    CHECK(builder.metadata().album() == "Album");
    CHECK(builder.property().fileSize() == 1000);
    CHECK(builder.property().bitDepth() == 16);
    CHECK(builder.tags().names().size() == 2);
    CHECK(builder.custom().pairs().size() == 1);
  }

  TEST_CASE("TrackBuilder - Serialize Empty Builder")
  {
    auto builder = TrackBuilder::createNew();
    auto const [hotData, coldData] = serializeTestTrack(builder);

    CHECK(hotData.size() >= sizeof(TrackHotHeader));
    CHECK(!hotData.empty());
  }

  TEST_CASE("TrackBuilder - Serialize With Strings")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().title("Hello World").year(2021);
    builder.property().uri("/music/test.flac");

    auto const [hotData, coldData] = serializeTestTrack(builder);

    // Verify header size
    CHECK(hotData.size() >= sizeof(TrackHotHeader));

    // Parse the serialized data back
    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());

    CHECK(header->titleLen == 11); // "Hello World"
    CHECK(header->year == 2021);

    // Verify strings are in the payload
    auto const* payloadStart = reinterpret_cast<char const*>(hotData.data()) + sizeof(TrackHotHeader);
    CHECK(std::strncmp(payloadStart, "Hello World", 11) == 0);
  }

  TEST_CASE("TrackBuilder - buildHotHeader Method")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().year(1999).rating(5);
    builder.property().bitDepth(24);

    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
    auto wtxn = WriteTransaction{env};
    auto dict = DictionaryStore{lmdb::Database{wtxn, "dict"}, wtxn};
    auto resources = ResourceStore{lmdb::Database{wtxn, "resources"}};

    auto const [hotData, coldData] = builder.serialize(wtxn, dict, resources);
    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());

    CHECK(header->year == 1999);
    CHECK(header->bitDepth == 24);
    CHECK(header->rating == 5);
  }

  TEST_CASE("TrackBuilder - Serialize With Special Characters")
  {
    auto builder = TrackBuilder::createNew();
    auto const* title = "Test: \"Quotes\" & 'Apostrophes'";
    builder.metadata().title(title);

    auto const [hotData, coldData] = serializeTestTrack(builder);

    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());
    CHECK(header->titleLen == std::strlen(title));
  }

  TEST_CASE("TrackBuilder - Serialize Preserves Data")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().title("Test");
    builder.property().uri("/test").fileSize(12345).mtime(9876543210);

    auto const [hotData1, coldData1] = serializeTestTrack(builder);
    auto const [hotData2, coldData2] = serializeTestTrack(builder);

    // Multiple serializations should produce same size and content
    CHECK(hotData1.size() == hotData2.size());
    CHECK(hotData1 == hotData2);
  }

  TEST_CASE("TrackBuilder - Tag Serialization - Empty Tags")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().title("Test");
    builder.property().uri("/test");

    auto const [hotData, coldData] = serializeTestTrack(builder);

    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());
    CHECK(header->tagLen == 0);
    CHECK(header->tagBloom == 0);
  }

  TEST_CASE("TrackBuilder - Tag Serialization - With Tags")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().title("Test");
    builder.property().uri("/test");
    builder.tags().add("tag1").add("tag2").add("tag3");

    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
    auto wtxn = WriteTransaction{env};
    auto dict = DictionaryStore{lmdb::Database{wtxn, "dict"}, wtxn};
    auto resources = ResourceStore{lmdb::Database{wtxn, "resources"}};
    auto const [hotData, coldData] = builder.serialize(wtxn, dict, resources);

    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());
    CHECK(header->tagLen == 12);  // 3 tags * 4 bytes each
    CHECK(header->tagBloom != 0); // Bloom should be computed from tag IDs
  }

  TEST_CASE("TrackBuilder - Tag Serialization - Single Tag")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().title("Test");
    builder.property().uri("/test");
    builder.tags().add("tag42");

    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
    auto wtxn = WriteTransaction{env};
    auto dict = DictionaryStore{lmdb::Database{wtxn, "dict"}, wtxn};
    auto resources = ResourceStore{lmdb::Database{wtxn, "resources"}};
    auto const [hotData, coldData] = builder.serialize(wtxn, dict, resources);

    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());
    CHECK(header->tagLen == 4); // 1 tag * 4 bytes
  }

  TEST_CASE("TrackBuilder - Tag Bloom Filter With Tags")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().title("Test");
    builder.property().uri("/test");
    builder.tags().add("tag1").add("tag2").add("tag3").add("tag4").add("tag5");

    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
    auto wtxn = WriteTransaction{env};
    auto dict = DictionaryStore{lmdb::Database{wtxn, "dict"}, wtxn};
    auto resources = ResourceStore{lmdb::Database{wtxn, "resources"}};
    auto const [hotData, coldData] = builder.serialize(wtxn, dict, resources);

    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());
    CHECK(header->tagLen == 20); // 5 tags * 4 bytes each
    CHECK(header->tagBloom != 0);
  }

  TEST_CASE("TrackBuilder - buildColdHeader")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().trackNumber(5).totalTracks(10).discNumber(1).totalDiscs(2);
    builder.property().uri("/path/to/file.flac").fileSize(2000).mtime(1234567890);

    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
    auto wtxn = WriteTransaction{env};
    auto dict = DictionaryStore{lmdb::Database{wtxn, "dict"}, wtxn};
    auto resources = ResourceStore{lmdb::Database{wtxn, "resources"}};
    auto const [hotData, coldData] = builder.serialize(wtxn, dict, resources);

    auto const* header = reinterpret_cast<TrackColdHeader const*>(coldData.data());
    CHECK(header->fileSizeLo == static_cast<std::uint32_t>(2000 & 0xFFFFFFFF));
    CHECK(header->fileSizeHi == static_cast<std::uint32_t>(static_cast<std::uint64_t>(2000) >> 32));
    CHECK(header->mtimeLo == static_cast<std::uint32_t>(1234567890 & 0xFFFFFFFF));
    CHECK(header->mtimeHi == static_cast<std::uint32_t>(static_cast<std::uint64_t>(1234567890) >> 32));
    CHECK(header->trackNumber == 5);
    CHECK(header->totalTracks == 10);
    CHECK(header->discNumber == 1);
    CHECK(header->totalDiscs == 2);
  }

  TEST_CASE("TrackBuilder - serializeHot")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().title("Test Title");
    builder.property().uri("/path/to/file.flac").fileSize(1000);
    builder.tags().add("tag10").add("tag20");

    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
    auto wtxn = WriteTransaction{env};
    auto dict = DictionaryStore{lmdb::Database{wtxn, "dict"}, wtxn};
    auto const hotData = builder.serializeHot(wtxn, dict);

    // Verify hot header
    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());
    CHECK(header->tagLen == 8); // 2 tags * 4 bytes

    // Verify bloom is computed
    CHECK(header->tagBloom != 0);
  }

  TEST_CASE("TrackBuilder - serializeCold")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().trackNumber(3);
    builder.property().uri("/path/to/file.flac").fileSize(2000).mtime(9876543210);
    builder.custom().add("key1", "value1").add("key2", "value2");

    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
    auto wtxn = WriteTransaction{env};
    auto dict = DictionaryStore{lmdb::Database{wtxn, "dict"}, wtxn};
    auto resources = ResourceStore{lmdb::Database{wtxn, "resources"}};
    auto const coldData = builder.serializeCold(wtxn, dict, resources);

    // Verify cold view can parse it
    auto view = TrackView{std::span<std::byte const>{}, coldData};
    CHECK(view.property().fileSize() == 2000);
    CHECK(view.property().mtime() == 9876543210);
    CHECK(view.metadata().trackNumber() == 3);
  }
} // namespace ao::library::test
