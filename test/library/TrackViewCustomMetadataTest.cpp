// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/library/TrackBuilderTestSupport.h"
#include "test/library/TrackViewTestSupport.h"
#include "test/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackView.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::library::test
{
  namespace
  {
#if defined(__GNUC__) && !defined(__clang__)
    static_assert(std::ranges::view<CustomMetadataProxy>);
#endif
  } // namespace

  TEST_CASE("TrackView - iterates no custom metadata when empty")
  {
    auto const data = makeColdTrackViewData();
    auto const view = makeColdTrackView(data);

    std::int32_t count = 0;

    for ([[maybe_unused]] auto const& [k, v] : view.customMetadata())
    {
      ++count;
    }

    CHECK(count == 0);
  }

  TEST_CASE("TrackView - iterates one custom metadata pair alongside a URI")
  {
    auto const pairs = std::vector{std::pair<std::string, std::string>{"key1", "value1"}};
    auto const data = makeColdTrackViewData({}, pairs, "/path/to/file.flac");
    auto const view = makeColdTrackView(data);

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

  TEST_CASE("TrackView - iterates multiple custom metadata pairs in stored order")
  {
    auto const key1 = std::string{"replaygain_track_gain_db"};
    auto const key2 = std::string{"isrc"};
    auto const key3 = std::string{"edition"};
    auto const pairs = std::vector{std::pair{key1, std::string{"-6.5"}},
                                   std::pair{key2, std::string{"USSM19999999"}},
                                   std::pair{key3, std::string{"remaster"}}};

    auto const data = makeColdTrackViewData({}, pairs, "/path/to/file.flac");
    auto const view = makeColdTrackView(data);

    // DictionaryStore assigns sequential IDs: key1->1, key2->2, key3->3
    auto const id0 = DictionaryId{1};
    auto const id1 = DictionaryId{2};
    auto const id2 = DictionaryId{3};

    auto result = std::vector<std::pair<DictionaryId, std::string_view>>{};

    for (auto const& [k, v] : view.customMetadata())
    {
      result.emplace_back(k, v);
    }

    REQUIRE(result.size() == 3);
    CHECK(result[0].first == id0);
    CHECK(result[0].second == "-6.5");
    CHECK(result[1].first == id1);
    CHECK(result[1].second == "USSM19999999");
    CHECK(result[2].first == id2);
    CHECK(result[2].second == "remaster");
  }

  TEST_CASE("TrackView - iterates custom metadata with special characters")
  {
    auto const pairs = std::vector{std::pair<std::string, std::string>{"comment", "Hello, World! 你好"}};

    auto const data = makeColdTrackViewData({}, pairs, "");
    auto const view = makeColdTrackView(data);

    std::size_t count = 0;

    for (auto const& [key, value] : view.customMetadata())
    {
      CHECK(key == DictionaryId{1});
      CHECK(value == "Hello, World! 你好");
      ++count;
    }

    CHECK(count == 1);
  }

  TEST_CASE("TrackView - finds custom metadata by key")
  {
    auto const pairs = std::vector{std::pair<std::string, std::string>{"replaygain_track_gain_db", "-6.5"},
                                   std::pair<std::string, std::string>{"isrc", "USSM19999999"}};

    auto const data = makeColdTrackViewData({}, pairs, "");
    auto const view = makeColdTrackView(data);

    // DictionaryStore assigns: replaygain->1, isrc->2
    auto const optValue = view.customMetadata().get(DictionaryId{2});
    REQUIRE(optValue);
    CHECK(*optValue == "USSM19999999");
  }

  TEST_CASE("TrackView - copied custom metadata survives backing record retirement")
  {
    auto copiedValue = std::string{};

    {
      auto const pairs = std::vector{std::pair<std::string, std::string>{"isrc", "USSM19999999"}};
      auto const data = makeColdTrackViewData({}, pairs, "");
      auto const view = makeColdTrackView(data);
      auto const optValue = view.customMetadata().get(DictionaryId{1});
      REQUIRE(optValue);
      copiedValue.assign(*optValue);
    }

    CHECK(copiedValue == "USSM19999999");
  }

  TEST_CASE("TrackView - returns empty custom metadata for missing keys")
  {
    auto const pairs = std::vector{std::pair<std::string, std::string>{"replaygain_track_gain_db", "-6.5"}};

    auto const data = makeColdTrackViewData({}, pairs, "");
    auto const view = makeColdTrackView(data);

    // ID 99 was never assigned
    auto const optValue = view.customMetadata().get(DictionaryId{99});
    CHECK_FALSE(optValue);
  }

  TEST_CASE("TrackView - finds custom metadata keys case sensitively")
  {
    auto const pairs = std::vector{std::pair<std::string, std::string>{"ISRC", "USSM19999999"},
                                   std::pair<std::string, std::string>{"isrc", "GBAYE6800011"}};

    auto const data = makeColdTrackViewData({}, pairs, "");
    auto const view = makeColdTrackView(data);

    REQUIRE(view.customMetadata().count() == 2);
    auto const optUppercaseValue = view.customMetadata().get(DictionaryId{1});
    REQUIRE(optUppercaseValue);
    CHECK(*optUppercaseValue == "USSM19999999");
    auto const optLowercaseValue = view.customMetadata().get(DictionaryId{2});
    REQUIRE(optLowercaseValue);
    CHECK(*optLowercaseValue == "GBAYE6800011");
  }

  TEST_CASE("TrackView - finds exact custom metadata keys on both sides of the search cutoff")
  {
    auto const count = GENERATE(63U, 64U, 65U);
    auto const lastId = (2 * count) + 1;
    auto fixture = TrackSerializationFixture{};
    auto& dictionary = physicalDictionary(fixture.transaction());
    auto pairs = std::vector<std::pair<std::string, std::string>>{};

    // Intern every key, but store only even IDs in the Track.
    for (std::uint32_t id = 1; id <= lastId; ++id)
    {
      auto const key = std::format("key{}", id);
      auto const idRes = dictionary.intern(key);
      REQUIRE(idRes);
      REQUIRE(*idRes == DictionaryId{id});

      if (id % 2 == 0)
      {
        pairs.emplace_back(key, std::format("value{}", id));
      }
    }

    auto builder = TrackBuilder::makeEmpty();
    builder.property().uri("track.flac");

    for (auto const& [key, value] : pairs)
    {
      builder.customMetadata().add(key, value);
    }

    auto const data = fixture.serializeCold(builder);
    auto const custom = makeColdTrackView(data).customMetadata();
    REQUIRE(custom.count() == count);

    for (std::uint32_t id = 0; id <= lastId; ++id)
    {
      CAPTURE(id);

      if (auto const key = DictionaryId{id}; id != 0 && id % 2 == 0)
      {
        CHECK(custom.contains(key));
        CHECK(custom.get(key) == std::format("value{}", id));
      }
      else
      {
        CHECK_FALSE(custom.contains(key));
        CHECK_FALSE(custom.get(key));
      }
    }
  }

  TEST_CASE("TrackView - preserves empty custom metadata keys and values")
  {
    auto const pairs = std::vector{std::pair<std::string, std::string>{"", ""}};

    auto const data = makeColdTrackViewData({}, pairs, "");
    auto const view = makeColdTrackView(data);

    std::int32_t count = 0;

    for (auto const& [k, v] : view.customMetadata())
    {
      CHECK(k == DictionaryId{1});
      CHECK(v.empty());
      ++count;
    }

    CHECK(count == 1);
    CHECK(view.customMetadata().contains(DictionaryId{1}));
    auto const optValue = view.customMetadata().get(DictionaryId{1});
    REQUIRE(optValue);
    CHECK(optValue->empty());
  }
} // namespace ao::library::test
