// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/lmdb/TestUtils.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/Type.h>
#include <ao/library/CoverArt.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::library::test
{
  using namespace ao::lmdb;
  using namespace ao::lmdb::test;

  namespace
  {
    class TrackSerializationContext final
    {
    public:
      TrackSerializationContext()
        : _env{openEnvironment(_temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20})}
        , _txn{beginWriteTransaction(_env)}
        , _dict{lmdb::test::openDatabase(_txn, "dict"), _txn}
        , _resources{lmdb::test::openDatabase(_txn, "resources")}
      {
      }

      std::pair<std::vector<std::byte>, std::vector<std::byte>> serialize(TrackBuilder& builder)
      {
        auto result = builder.serialize(_txn, _dict, _resources);
        REQUIRE(result);
        return *result;
      }

      Result<std::vector<std::byte>> trySerializeCold(TrackBuilder& builder)
      {
        return builder.serializeCold(_txn, _dict, _resources);
      }

      std::vector<std::byte> serializeCold(TrackBuilder& builder)
      {
        auto result = trySerializeCold(builder);
        REQUIRE(result);
        return *result;
      }

    private:
      ao::test::TempDir _temp;
      Environment _env;
      WriteTransaction _txn;
      DictionaryStore _dict;
      ResourceStore _resources;
    };

    std::pair<std::vector<std::byte>, std::vector<std::byte>> serializeTestTrack(TrackBuilder& builder)
    {
      auto context = TrackSerializationContext{};
      return context.serialize(builder);
    }
  } // namespace

  TEST_CASE("TrackBuilder - createNew returns an empty builder", "[library][unit][track]")
  {
    auto builder = TrackBuilder::createNew();

    CHECK(builder.metadata().title().empty());
    CHECK(builder.metadata().artist().empty());
    CHECK(builder.metadata().album().empty());
    CHECK(builder.metadata().albumArtist().empty());
    CHECK(builder.metadata().composer().empty());
    CHECK(builder.metadata().genre().empty());
    CHECK(builder.property().uri().empty());
    CHECK(builder.property().bitDepth() == 0);
    CHECK(builder.property().duration() == std::chrono::milliseconds{0});
    CHECK(builder.tags().names().empty());
    CHECK(builder.coverArt().entries().empty());
    CHECK(builder.customMetadata().pairs().empty());
  }

  TEST_CASE("TrackBuilder - cover art builder edits ordered entries", "[library][unit][track][cover]")
  {
    auto builder = TrackBuilder::createNew();
    auto const data = std::array{std::byte{0x12}, std::byte{0x34}};

    builder.coverArt().add(PictureType::BackCover, ResourceId{41}).add(PictureType::FrontCover, data);

    REQUIRE(builder.coverArt().entries().size() == 2);
    CHECK(builder.coverArt().entries()[0].type == PictureType::BackCover);
    CHECK(std::get<ResourceId>(builder.coverArt().entries()[0].source) == ResourceId{41});
    CHECK(std::ranges::equal(std::get<std::span<std::byte const>>(builder.coverArt().entries()[1].source), data));

    builder.coverArt().erase(0);

    REQUIRE(builder.coverArt().entries().size() == 1);
    CHECK(builder.coverArt().entries()[0].type == PictureType::FrontCover);

    builder.coverArt().clear();
    CHECK(builder.coverArt().entries().empty());

    builder.coverArt()
      .add(PictureType::Other, kInvalidResourceId)
      .add(PictureType::Other, std::span<std::byte const>{});
    CHECK(builder.coverArt().entries().empty());
  }

  TEST_CASE("TrackBuilder - metadata builder fluent setters update fields", "[library][unit][track]")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata()
      .title("Test Title")
      .artist("Test Artist")
      .album("Test Album")
      .albumArtist("Test Album Artist")
      .composer("Test Composer")
      .genre("Rock")
      .work("Symphony No. 9")
      .movement("I. Allegro ma non troppo")
      .year(2024)
      .trackNumber(5)
      .trackTotal(10)
      .discNumber(2)
      .discTotal(3)
      .movementNumber(1)
      .movementTotal(4);
    builder.coverArt().add(PictureType::FrontCover, ResourceId{42});

    CHECK(builder.metadata().title() == "Test Title");
    CHECK(builder.metadata().artist() == "Test Artist");
    CHECK(builder.metadata().album() == "Test Album");
    CHECK(builder.metadata().albumArtist() == "Test Album Artist");
    CHECK(builder.metadata().composer() == "Test Composer");
    CHECK(builder.metadata().genre() == "Rock");
    CHECK(builder.metadata().work() == "Symphony No. 9");
    CHECK(builder.metadata().movement() == "I. Allegro ma non troppo");
    CHECK(builder.metadata().year() == 2024);
    CHECK(builder.metadata().trackNumber() == 5);
    CHECK(builder.metadata().trackTotal() == 10);
    CHECK(builder.metadata().discNumber() == 2);
    CHECK(builder.metadata().discTotal() == 3);
    CHECK(builder.metadata().movementNumber() == 1);
    CHECK(builder.metadata().movementTotal() == 4);
    REQUIRE(builder.coverArt().entries().size() == 1);
    CHECK(std::get<ResourceId>(builder.coverArt().entries()[0].source) == ResourceId{42});
    CHECK(builder.coverArt().entries()[0].type == PictureType::FrontCover);

    builder.coverArt().clear();
    CHECK(builder.coverArt().entries().empty());
  }

  TEST_CASE("TrackBuilder - property builder fluent setters update fields", "[library][unit][track]")
  {
    auto builder = TrackBuilder::createNew();
    builder.property()
      .uri("file:///home/user/music/test.flac")
      .duration(std::chrono::minutes{3} + std::chrono::milliseconds{500})
      .bitrate(Bitrate{320000})
      .sampleRate(SampleRate{44100})
      .codec(AudioCodec::Alac)
      .channels(Channels{2})
      .bitDepth(BitDepth{16});

    CHECK(builder.property().uri() == "file:///home/user/music/test.flac");
    CHECK(builder.property().duration() == std::chrono::minutes{3} + std::chrono::milliseconds{500});
    CHECK(builder.property().bitrate() == 320000);
    CHECK(builder.property().sampleRate() == 44100);
    CHECK(builder.property().codec() == AudioCodec::Alac);
    CHECK(builder.property().channels() == 2);
    CHECK(builder.property().bitDepth() == 16);
  }

  TEST_CASE("TrackBuilder - tags builder adds removes and clears names", "[library][unit][track]")
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
    CHECK(std::ranges::contains(builder.tags().names(), std::string_view{"rock"}));
    CHECK(std::ranges::contains(builder.tags().names(), std::string_view{"blues"}));

    // Clear
    builder.tags().clear();
    CHECK(builder.tags().names().empty());
  }

  TEST_CASE("TrackBuilder - custom metadata builder adds removes and clears pairs", "[library][unit][track]")
  {
    auto builder = TrackBuilder::createNew();

    // Add custom pairs
    builder.customMetadata().add("replaygain_track_gain_db", "-6.5");
    builder.customMetadata().add("isrc", "USSM19999999");

    CHECK(builder.customMetadata().pairs().size() == 2);

    // Remove a pair
    builder.customMetadata().remove("isrc");
    CHECK(builder.customMetadata().pairs().size() == 1);
    CHECK(builder.customMetadata().pairs()[0].first == "replaygain_track_gain_db");
    CHECK(builder.customMetadata().pairs()[0].second == "-6.5");

    // Clear
    builder.customMetadata().clear();
    CHECK(builder.customMetadata().pairs().empty());
  }

  TEST_CASE("TrackBuilder - chained API updates builder state", "[library][unit][track]")
  {
    auto builder = TrackBuilder::createNew();

    builder.metadata().title("Song").artist("Artist").album("Album");
    builder.property().bitDepth(BitDepth{16});
    builder.tags().add("rock").add("jazz");
    builder.customMetadata().add("key", "value");

    CHECK(builder.metadata().title() == "Song");
    CHECK(builder.metadata().artist() == "Artist");
    CHECK(builder.metadata().album() == "Album");
    CHECK(builder.property().bitDepth() == 16);
    CHECK(builder.tags().names().size() == 2);
    CHECK(builder.customMetadata().pairs().size() == 1);
  }

  TEST_CASE("TrackBuilder - serializes empty builders", "[library][unit][track]")
  {
    auto builder = TrackBuilder::createNew();
    auto const [hotData, coldData] = serializeTestTrack(builder);

    CHECK(hotData.size() >= sizeof(TrackHotHeader));
    CHECK(!hotData.empty());
  }

  TEST_CASE("TrackBuilder - serializes strings into hot payloads", "[library][unit][track]")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().title("Hello World").year(2021);
    builder.property().uri("/music/test.flac");

    auto const [hotData, coldData] = serializeTestTrack(builder);

    // Verify header size
    CHECK(hotData.size() >= sizeof(TrackHotHeader));

    // Parse the serialized data back
    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());

    CHECK(header->titleLength == 11); // "Hello World"
    CHECK(header->year == 2021);

    // Verify strings are in the payload
    auto const* payloadStart = reinterpret_cast<char const*>(hotData.data()) + sizeof(TrackHotHeader);
    CHECK(std::strncmp(payloadStart, "Hello World", 11) == 0);
  }

  TEST_CASE("TrackBuilder - serialize writes hot header fields", "[library][unit][track]")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().year(1999);
    builder.property().bitDepth(BitDepth{24});

    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto dict = DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};
    auto resources = ResourceStore{lmdb::test::openDatabase(wtxn, "resources")};

    auto serializeResult = builder.serialize(wtxn, dict, resources);
    REQUIRE(serializeResult);
    auto const [hotData, coldData] = *serializeResult;
    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());

    CHECK(header->year == 1999);
    CHECK(header->bitDepth == 24);
  }

  TEST_CASE("TrackBuilder - serializes strings with special characters", "[library][unit][track]")
  {
    auto builder = TrackBuilder::createNew();
    auto const* title = "Test: \"Quotes\" & 'Apostrophes'";
    builder.metadata().title(title);

    auto const [hotData, coldData] = serializeTestTrack(builder);

    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());
    CHECK(header->titleLength == std::strlen(title));
  }

  TEST_CASE("TrackBuilder - serialization is stable across repeated calls", "[library][unit][track]")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().title("Test");
    builder.property().uri("/test");

    auto const [hotData1, coldData1] = serializeTestTrack(builder);
    auto const [hotData2, coldData2] = serializeTestTrack(builder);

    // Multiple serializations should produce same size and content
    CHECK(hotData1.size() == hotData2.size());
    CHECK(hotData1 == hotData2);
  }

  TEST_CASE("TrackBuilder - serializes empty tag data", "[library][unit][track]")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().title("Test");
    builder.property().uri("/test");

    auto const [hotData, coldData] = serializeTestTrack(builder);

    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());
    CHECK(header->tagLength == 0);
    CHECK(header->tagBloom == 0);
  }

  TEST_CASE("TrackBuilder - serializes multiple tags", "[library][unit][track]")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().title("Test");
    builder.property().uri("/test");
    builder.tags().add("tag1").add("tag2").add("tag3");

    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto dict = DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};
    auto resources = ResourceStore{lmdb::test::openDatabase(wtxn, "resources")};
    auto serializeResult = builder.serialize(wtxn, dict, resources);
    REQUIRE(serializeResult);
    auto const [hotData, coldData] = *serializeResult;

    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());
    CHECK(header->tagLength == 12); // 3 tags * 4 bytes each
    CHECK(header->tagBloom != 0);   // Bloom should be computed from tag IDs
  }

  TEST_CASE("TrackBuilder - serializes one tag", "[library][unit][track]")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().title("Test");
    builder.property().uri("/test");
    builder.tags().add("tag42");

    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto dict = DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};
    auto resources = ResourceStore{lmdb::test::openDatabase(wtxn, "resources")};
    auto serializeResult = builder.serialize(wtxn, dict, resources);
    REQUIRE(serializeResult);
    auto const [hotData, coldData] = *serializeResult;

    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());
    CHECK(header->tagLength == 4); // 1 tag * 4 bytes
  }

  TEST_CASE("TrackBuilder - computes tag bloom filters with tags", "[library][unit][track]")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().title("Test");
    builder.property().uri("/test");
    builder.tags().add("tag1").add("tag2").add("tag3").add("tag4").add("tag5");

    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto dict = DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};
    auto resources = ResourceStore{lmdb::test::openDatabase(wtxn, "resources")};
    auto serializeResult = builder.serialize(wtxn, dict, resources);
    REQUIRE(serializeResult);
    auto const [hotData, coldData] = *serializeResult;

    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());
    CHECK(header->tagLength == 20); // 5 tags * 4 bytes each
    CHECK(header->tagBloom != 0);
  }

  TEST_CASE("TrackBuilder - serialize writes cold header fields", "[library][unit][track]")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().trackNumber(5).trackTotal(10).discNumber(1).discTotal(2);
    builder.property().uri("/path/to/file.flac").duration(std::chrono::minutes{3});

    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto dict = DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};
    auto resources = ResourceStore{lmdb::test::openDatabase(wtxn, "resources")};
    auto serializeResult = builder.serialize(wtxn, dict, resources);
    REQUIRE(serializeResult);
    auto const [hotData, coldData] = *serializeResult;

    auto const* header = reinterpret_cast<TrackColdHeader const*>(coldData.data());
    CHECK(header->duration == std::chrono::minutes{3});
    CHECK(header->trackNumber == 5);
    CHECK(header->trackTotal == 10);
    CHECK(header->discNumber == 1);
    CHECK(header->discTotal == 2);
  }

  TEST_CASE("TrackBuilder - serializeHot writes tag header data", "[library][unit][track]")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().title("Test Title");
    builder.property().uri("/path/to/file.flac");
    builder.tags().add("tag10").add("tag20");

    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto dict = DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};
    auto hotDataResult = builder.serializeHot(wtxn, dict);
    REQUIRE(hotDataResult);
    auto const& hotData = *hotDataResult;

    // Verify hot header
    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());
    CHECK(header->tagLength == 8); // 2 tags * 4 bytes

    // Verify bloom is computed
    CHECK(header->tagBloom != 0);
  }

  TEST_CASE("TrackBuilder - serializeCold writes cold view data", "[library][unit][track]")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().trackNumber(3);
    builder.property().uri("/path/to/file.flac").duration(std::chrono::minutes{4});
    builder.customMetadata().add("key1", "value1").add("key2", "value2");

    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto dict = DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};
    auto resources = ResourceStore{lmdb::test::openDatabase(wtxn, "resources")};
    auto coldDataResult = builder.serializeCold(wtxn, dict, resources);
    REQUIRE(coldDataResult);
    auto const& coldData = *coldDataResult;

    // Verify cold view can parse it
    auto view = TrackView{std::span<std::byte const>{}, coldData};
    CHECK(view.property().duration() == std::chrono::minutes{4});
    CHECK(view.metadata().trackNumber() == 3);
  }

  TEST_CASE("TrackBuilder - aligns cover table after custom values", "[library][unit][track][cover]")
  {
    auto builder = TrackBuilder::createNew();
    builder.property().uri("song.flac");
    builder.customMetadata().add("odd", "abc");
    builder.coverArt().add(PictureType::BackCover, ResourceId{41});
    builder.coverArt().add(PictureType::FrontCover, ResourceId{42});

    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto dict = DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};
    auto resources = ResourceStore{lmdb::test::openDatabase(wtxn, "resources")};
    auto coldDataResult = builder.serializeCold(wtxn, dict, resources);
    REQUIRE(coldDataResult);
    auto const& coldData = *coldDataResult;

    auto const view = TrackView{std::span<std::byte const>{}, coldData};
    REQUIRE(view.coverArt().count() == 2);
    CHECK(view.coverArt().at(0).type == PictureType::BackCover);
    CHECK(view.coverArt().at(1).type == PictureType::FrontCover);
    REQUIRE(view.coverArt().primary());
    CHECK(view.coverArt().primary()->resourceId == ResourceId{42});

    auto iterated = std::vector<CoverArt>{};

    for (auto const cover : view.coverArt())
    {
      iterated.push_back(cover);
    }

    REQUIRE(iterated.size() == 2);
    CHECK(iterated[0].resourceId == ResourceId{41});
    CHECK(iterated[1].resourceId == ResourceId{42});
  }

  TEST_CASE("TrackBuilder - fromView reconstructs builder fields", "[library][unit][track]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto dict = DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};
    auto resources = ResourceStore{lmdb::test::openDatabase(wtxn, "resources")};

    auto original = TrackBuilder::createNew();
    original.metadata().title("Title").albumArtist("Test Album Artist").composer("Test Composer");
    original.property().uri("/path.flac");

    auto originalSerializeResult = original.serialize(wtxn, dict, resources);
    REQUIRE(originalSerializeResult);
    auto const [hotData, coldData] = *originalSerializeResult;
    auto view = TrackView{hotData, coldData};

    auto reconstructed = TrackBuilder::fromView(view, dict);
    CHECK(reconstructed.metadata().title() == "Title");
    CHECK(reconstructed.metadata().albumArtist() == "Test Album Artist");
    CHECK(reconstructed.metadata().composer() == "Test Composer");
    CHECK(reconstructed.property().uri() == "/path.flac");

    // test const getters
    auto const& constBuilder = reconstructed;
    CHECK(constBuilder.property().uri() == "/path.flac");
    CHECK(constBuilder.tags().names().empty());
  }

  TEST_CASE("TrackBuilder - serialized views expose property and metadata fields", "[library][unit][track]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto dict = DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};
    auto resources = ResourceStore{lmdb::test::openDatabase(wtxn, "resources")};

    auto builder = TrackBuilder::createNew();
    builder.metadata()
      .trackNumber(1)
      .trackTotal(10)
      .discNumber(2)
      .discTotal(3)
      .album("Album")
      .genre("Genre")
      .albumArtist("Album Artist");
    builder.coverArt().add(PictureType::FrontCover, ResourceId{42});
    builder.tags().add("tag1").add("tag2");

    auto serializeResult = builder.serialize(wtxn, dict, resources);
    REQUIRE(serializeResult);
    auto const [hotData, coldData] = *serializeResult;
    auto view = TrackView{hotData, coldData};

    CHECK(view.metadata().trackNumber() == 1);
    CHECK(view.metadata().trackTotal() == 10);
    CHECK(view.metadata().discNumber() == 2);
    CHECK(view.metadata().discTotal() == 3);
    REQUIRE(view.coverArt().primary());
    CHECK(view.coverArt().primary()->resourceId == ResourceId{42});
    CHECK(view.metadata().albumId() == dict.getId("Album"));
    CHECK(view.metadata().genreId() == dict.getId("Genre"));
    CHECK(view.metadata().albumArtistId() == dict.getId("Album Artist"));
  }

  TEST_CASE("TrackBuilder - cold serialization rejects values that exceed header fields", "[library][unit][track]")
  {
    constexpr std::size_t kUint16Max = std::numeric_limits<std::uint16_t>::max();
    constexpr auto kUint16Overflow = kUint16Max + 1;

    auto context = TrackSerializationContext{};

    SECTION("URI length")
    {
      auto builder = TrackBuilder::createNew();
      auto const uri = std::string(kUint16Overflow, 'u');
      builder.property().uri(uri);

      auto const result = context.trySerializeCold(builder);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);
    }

    SECTION("Custom metadata value length")
    {
      auto builder = TrackBuilder::createNew();
      auto const value = std::string(kUint16Overflow, 'v');
      builder.customMetadata().add("key", value);

      auto const result = context.trySerializeCold(builder);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);
    }

    SECTION("Cover art count")
    {
      auto builder = TrackBuilder::createNew();

      for (std::size_t i = 0; i < kUint16Overflow; ++i)
      {
        builder.coverArt().add(PictureType::Other, ResourceId{static_cast<std::uint32_t>(i + 1)});
      }

      auto const result = context.trySerializeCold(builder);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);
    }

    SECTION("Custom metadata count")
    {
      auto builder = TrackBuilder::createNew();

      for (std::size_t i = 0; i < kUint16Overflow; ++i)
      {
        builder.customMetadata().add("key", {});
      }

      auto const result = context.trySerializeCold(builder);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);
    }

    SECTION("Custom metadata table offset")
    {
      constexpr std::size_t kOverflowCount = (kUint16Overflow / sizeof(CoverArtEntry));

      auto builder = TrackBuilder::createNew();

      for (std::size_t i = 0; i < kOverflowCount; ++i)
      {
        builder.coverArt().add(PictureType::Other, ResourceId{static_cast<std::uint32_t>(i + 1)});
      }

      auto const result = context.trySerializeCold(builder);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);
    }

    SECTION("URI offset after accumulated custom metadata values")
    {
      constexpr std::size_t kValueCount = 2;
      constexpr std::size_t kValueSize =
        (kUint16Overflow - sizeof(TrackColdHeader) - (kValueCount * sizeof(CustomMetadataEntry))) / kValueCount;

      auto builder = TrackBuilder::createNew();
      auto const value = std::string(kValueSize, 'v');

      builder.customMetadata().add("first", value).add("second", value);

      auto const result = context.trySerializeCold(builder);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);
    }
  }
} // namespace ao::library::test
