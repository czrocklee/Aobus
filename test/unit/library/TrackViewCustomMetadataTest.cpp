// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/lmdb/TestUtils.h"
#include <ao/Type.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::library::test
{
  namespace
  {
#if defined(__GNUC__) && !defined(__clang__)
    static_assert(std::ranges::view<TrackView::CustomMetadataProxy>);
#endif

    using namespace ao::lmdb::test;

    std::vector<std::byte> createColdData(TrackColdHeader const& header = {},
                                          std::vector<std::pair<std::string, std::string>> const& customPairs = {},
                                          std::string_view uri = "")
    {
      auto builder = TrackBuilder::createNew();
      builder.property().uri(uri);
      builder.metadata().trackNumber(header.trackNumber);
      builder.metadata().trackTotal(header.trackTotal);
      builder.metadata().discNumber(header.discNumber);
      builder.metadata().discTotal(header.discTotal);
      builder.property().duration(header.duration);
      builder.property().bitrate(header.bitrate);
      builder.property().channels(header.channels);

      for (auto const& [key, value] : customPairs)
      {
        builder.customMetadata().add(key, value);
      }

      auto temp = ao::test::TempDir{};
      auto env = lmdb::test::openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
      auto wtxn = lmdb::test::beginWriteTransaction(env);
      auto dict = DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};
      auto resources = ResourceStore{lmdb::test::openDatabase(wtxn, "resources")};
      auto result = builder.serializeCold(wtxn, dict, resources);
      REQUIRE(result);
      return *result;
    }

    TrackView makeColdView(std::vector<std::byte> const& data)
    {
      return TrackView{std::span<std::byte const>{}, data};
    }
  } // namespace

  TEST_CASE("TrackView - round-trips empty custom metadata", "[library][unit][track][custom-metadata]")
  {
    auto const data = createColdData();
    auto const view = makeColdView(data);

    std::int32_t count = 0;

    for ([[maybe_unused]] auto const& [k, v] : view.customMetadata())
    {
      ++count;
    }

    CHECK(count == 0);
  }

  TEST_CASE("TrackView - round-trips one custom metadata pair", "[library][unit][track][custom-metadata]")
  {
    auto const pairs = std::vector{std::pair<std::string, std::string>{"key1", "value1"}};
    auto const data = createColdData({}, pairs, "/path/to/file.flac");
    auto const view = makeColdView(data);

    std::int32_t count = 0;

    for (auto const& [k, v] : view.customMetadata())
    {
      CHECK(k == DictionaryId{1});
      CHECK(v == "value1");
      ++count;
    }

    CHECK(count == 1);
    CHECK(view.property().uri() == "/path/to/file.flac");
  }

  TEST_CASE("TrackView - round-trips multiple custom metadata pairs", "[library][unit][track][custom-metadata]")
  {
    auto const key1 = std::string{"replaygain_track_gain_db"};
    auto const key2 = std::string{"isrc"};
    auto const key3 = std::string{"edition"};
    auto const pairs = std::vector{std::pair{key1, std::string{"-6.5"}},
                                   std::pair{key2, std::string{"USSM19999999"}},
                                   std::pair{key3, std::string{"remaster"}}};

    auto const data = createColdData({}, pairs, "/path/to/file.flac");
    auto const view = makeColdView(data);

    // DictionaryStore assigns sequential IDs: key1->1, key2->2, key3->3
    auto const id0 = DictionaryId{1};
    auto const id1 = DictionaryId{2};
    auto const id2 = DictionaryId{3};

    auto result = std::vector<std::pair<DictionaryId, std::string_view>>{};

    for (auto const& [k, v] : view.customMetadata())
    {
      result.emplace_back(k, v);
    }

    CHECK(result.size() == 3);
    CHECK(result[0].first == id0);
    CHECK(result[0].second == "-6.5");
    CHECK(result[1].first == id1);
    CHECK(result[1].second == "USSM19999999");
    CHECK(result[2].first == id2);
    CHECK(result[2].second == "remaster");
  }

  TEST_CASE("TrackView - iterates no custom metadata when empty", "[library][unit][track][custom-metadata]")
  {
    auto const data = createColdData({}, {}, "");
    auto const view = makeColdView(data);

    std::int32_t count = 0;

    for ([[maybe_unused]] auto const& [key, value] : view.customMetadata())
    {
      ++count;
    }

    CHECK(count == 0);
  }

  TEST_CASE("TrackView - iterates one custom metadata pair", "[library][unit][track][custom-metadata]")
  {
    auto const pairs = std::vector{std::pair<std::string, std::string>{"key1", "value1"}};

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    std::int32_t count = 0;

    for (auto const& [key, value] : view.customMetadata())
    {
      CHECK(key == DictionaryId{1});
      CHECK(value == "value1");
      ++count;
    }

    CHECK(count == 1);
  }

  TEST_CASE("TrackView - iterates custom metadata with special characters", "[library][unit][track][custom-metadata]")
  {
    auto const pairs = std::vector{std::pair<std::string, std::string>{"comment", "Hello, World! 你好"}};

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    for (auto const& [key, value] : view.customMetadata())
    {
      CHECK(key == DictionaryId{1});
      CHECK(value == "Hello, World! 你好");
    }
  }

  TEST_CASE("TrackView - iterates multiple custom metadata pairs", "[library][unit][track][custom-metadata]")
  {
    auto const key1 = std::string{"replaygain_track_gain_db"};
    auto const key2 = std::string{"isrc"};
    auto const key3 = std::string{"edition"};
    auto const pairs = std::vector{std::pair{key1, std::string{"-6.5"}},
                                   std::pair{key2, std::string{"USSM19999999"}},
                                   std::pair{key3, std::string{"remaster"}}};

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    auto result = std::vector<std::pair<DictionaryId, std::string_view>>{};

    for (auto const& [key, value] : view.customMetadata())
    {
      result.emplace_back(key, value);
    }

    CHECK(result.size() == 3);
    CHECK(result[0].first == DictionaryId{1});
    CHECK(result[0].second == "-6.5");
    CHECK(result[1].first == DictionaryId{2});
    CHECK(result[1].second == "USSM19999999");
    CHECK(result[2].first == DictionaryId{3});
    CHECK(result[2].second == "remaster");
  }

  TEST_CASE("TrackView - finds custom metadata by key", "[library][unit][track][custom-metadata]")
  {
    auto const pairs = std::vector{std::pair<std::string, std::string>{"replaygain_track_gain_db", "-6.5"},
                                   std::pair<std::string, std::string>{"isrc", "USSM19999999"}};

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    // DictionaryStore assigns: replaygain->1, isrc->2
    auto const optValue = view.customMetadata().get(DictionaryId{2});
    REQUIRE(optValue.has_value());
    CHECK(*optValue == "USSM19999999");
  }

  TEST_CASE("TrackView - returns empty custom metadata for missing keys", "[library][unit][track][custom-metadata]")
  {
    auto const pairs = std::vector{std::pair<std::string, std::string>{"replaygain_track_gain_db", "-6.5"}};

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    // ID 99 was never assigned
    auto const optValue = view.customMetadata().get(DictionaryId{99});
    CHECK(optValue.has_value() == false);
  }

  TEST_CASE("TrackView - finds custom metadata keys case sensitively", "[library][unit][track][custom-metadata]")
  {
    auto const pairs = std::vector{std::pair<std::string, std::string>{"ISRC", "USSM19999999"}};

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    // "ISRC" is stored at ID 1, looking up by ID 1 returns the value
    auto const optValue = view.customMetadata().get(DictionaryId{1});
    REQUIRE(optValue.has_value());
    CHECK(*optValue == "USSM19999999");
  }

  TEST_CASE("TrackView - finds custom metadata through binary search", "[library][unit][track][custom-metadata]")
  {
    // Create 100 entries to force binary search path
    auto pairs = std::vector<std::pair<std::string, std::string>>{};

    for (std::int32_t i = 0; i < 100; ++i)
    {
      pairs.emplace_back(std::format("key{}", i), std::format("value{}", i));
    }

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    auto const optFirst = view.customMetadata().get(DictionaryId{1});
    REQUIRE(optFirst.has_value());
    CHECK(*optFirst == "value0");

    auto const optMiddle = view.customMetadata().get(DictionaryId{50});
    REQUIRE(optMiddle.has_value());
    CHECK(*optMiddle == "value49");

    auto const optLast = view.customMetadata().get(DictionaryId{100});
    REQUIRE(optLast.has_value());
    CHECK(*optLast == "value99");
    // Not found
    CHECK(view.customMetadata().get(DictionaryId{199}).has_value() == false);
    // Before first (0 = null, should throw)
    CHECK(view.customMetadata().get(kInvalidDictionaryId).has_value() == false);
  }

  TEST_CASE("TrackView - preserves empty custom metadata keys and values", "[library][unit][track][custom-metadata]")
  {
    auto const pairs = std::vector{std::pair<std::string, std::string>{"", ""}};

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    std::int32_t count = 0;

    for (auto const& [k, v] : view.customMetadata())
    {
      CHECK(k == DictionaryId{1});
      CHECK(v.empty());
      ++count;
    }

    CHECK(count == 1);
  }

  TEST_CASE("TrackView - preserves special characters in custom metadata values",
            "[library][unit][track][custom-metadata]")
  {
    auto const pairs = std::vector{std::pair<std::string, std::string>{"comment", "Hello, World! 你好"}};

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    for (auto const& [k, v] : view.customMetadata())
    {
      CHECK(k == DictionaryId{1});
      CHECK(v == "Hello, World! 你好");
    }
  }
} // namespace ao::library::test
