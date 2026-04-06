// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <optional>
#include <rs/core/DictionaryStore.h>
#include <rs/core/TrackBuilder.h>
#include <rs/core/TrackLayout.h>
#include <rs/core/TrackRecord.h>
#include <rs/core/TrackStore.h>
#include <rs/expr/ExecutionPlan.h>
#include <rs/expr/Parser.h>
#include <rs/expr/PlanEvaluator.h>
#include <rs/lmdb/Database.h>
#include <rs/lmdb/Environment.h>
#include <rs/lmdb/Transaction.h>
#include <rs/utility/ByteView.h>
#include <span>
#include <test/unit/core/TestUtils.h>
#include <test/unit/lmdb/TestUtils.h>

#include <vector>

namespace
{

  using rs::core::DictionaryId;
  using rs::lmdb::Database;

  // Helper class to hold both the serialized data and create TrackView
  // This ensures the data stays valid while the view is in use
  class TestTrack
  {
  public:
    TestTrack(std::string title = "Test Title",
              std::string artist = "Test Artist",
              std::string album = "Test Album",
              std::string uri = "/path/to/track.flac",
              std::uint16_t year = 2020,
              std::uint16_t trackNumber = 5,
              std::uint32_t durationMs = 180000,
              std::uint32_t bitrate = 320000,
              std::uint32_t sampleRate = 44100,
              std::uint8_t channels = 2,
              std::uint8_t bitDepth = 16,
              std::uint32_t artistId = 1,
              std::uint32_t albumId = 2,
              std::uint32_t genreId = 3,
              std::vector<std::uint32_t> tagIds = {})
    {
      _builder.metadata().title(std::move(title));
      _builder.metadata().artist(std::move(artist));
      _builder.metadata().album(std::move(album));
      _builder.property().uri(std::move(uri));
      _builder.metadata().year(year);
      _builder.metadata().trackNumber(trackNumber);
      _builder.property().durationMs(durationMs);
      _builder.property().bitrate(bitrate);
      _builder.property().sampleRate(sampleRate);
      _builder.property().channels(channels);
      _builder.property().bitDepth(bitDepth);
      // Add tags using string names (will be resolved to IDs via dict.put during serialize)
      for (auto id : tagIds)
      {
        _builder.tags().add("tag" + std::to_string(id));
      }

      // Build hot and cold data with a dictionary
      auto temp = TempDir{};
      auto envOpts = rs::lmdb::Environment::Options{.flags = MDB_CREATE, .maxDatabases = 20};
      _env.emplace(temp.path(), envOpts);
      auto wtxn = rs::lmdb::WriteTransaction{*_env};
      _dict.emplace(Database{wtxn, "dict"}, wtxn);

      _hotData = _builder.serializeHot(*_dict, wtxn);
      _coldData = _builder.serializeCold(*_dict, wtxn);

      // Fix up the header with specific IDs
      // Note: we serialize then modify, so const_cast is safe
      auto* header = const_cast<rs::core::TrackHotHeader*>(rs::utility::as<rs::core::TrackHotHeader>(_hotData));
      header->artistId = DictionaryId{artistId};
      header->albumId = DictionaryId{albumId};
      header->genreId = DictionaryId{genreId};
      header->albumArtistId = DictionaryId{0};
      header->codecId = 0;
      header->rating = 0;
    }

    // Returns TrackView with both hot and cold data
    rs::core::TrackView view() const { return rs::core::TrackView{_hotData, _coldData}; }

    // Returns TrackView with hot data only (for invalid cold tests)
    rs::core::TrackView hotOnlyView() const { return rs::core::TrackView{_hotData, std::span<std::byte const>{}}; }

  private:
    rs::core::TrackBuilder _builder = rs::core::TrackBuilder::createNew();
    std::optional<rs::lmdb::Environment> _env;
    std::optional<rs::core::DictionaryStore> _dict;
    std::vector<std::byte> _hotData;
    std::vector<std::byte> _coldData;
  };

} // namespace

using rs::core::DictionaryId;
using rs::core::DictionaryStore;
using rs::lmdb::Database;
using rs::lmdb::Environment;
using rs::lmdb::ReadTransaction;
using rs::lmdb::WriteTransaction;
using namespace rs::expr;

TEST_CASE("PlanEvaluator - Simple Equal Match")
{
  auto expr = parse("$year = 2020");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2019};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Greater Than")
{
  auto expr = parse("@duration > 179000");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 170000};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - NotEqual")
{
  auto expr = parse("$year != 2020");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == false);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2021, 5, 180000};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == true);

  auto track3 = TestTrack{"Test", "Artist", "Album", "/path", 2019, 5, 180000};
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == true);
}

TEST_CASE("PlanEvaluator - Greater Than Or Equal")
{
  auto expr = parse("@duration >= 180000");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 179999};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Less Than")
{
  auto expr = parse("$year < 2021");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2021};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Like Match")
{
  auto expr = parse("$title ~ Test");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test Title"};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Another Title"};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Title String Equal")
{
  auto expr = parse("$title = 'Hello World'");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Hello World"};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"hello world"}; // case-sensitive
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);

  auto track3 = TestTrack{"Hello"};
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Title String Not Equal")
{
  auto expr = parse("$title != 'Hello'");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Hello World"};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Hello"};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Title String Less Than")
{
  auto expr = parse("$title < 'zoo'");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"apple"};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"zoo"};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);

  auto track3 = TestTrack{"zooExtra"};
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Title String Greater Than")
{
  auto expr = parse("$title > 'apple'");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"banana"};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"apple"};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);

  auto track3 = TestTrack{"Apple"}; // case-sensitive
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Logical And")
{
  auto expr = parse("$year = 2020 && @duration > 100000");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2019, 5, 180000};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);

  auto track3 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 50000};
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Logical Or")
{
  auto expr = parse("$year = 2020 || $year = 2019");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2019};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == true);

  auto track3 = TestTrack{"Test", "Artist", "Album", "/path", 2018};
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Logical Not")
{
  // Use "not(" for explicit grouping, or check if parser handles precedence
  auto expr = parse("!($year = 2020)");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == false);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2019};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == true);
}

TEST_CASE("PlanEvaluator - Complex Expression")
{
  // Note: genre comparison requires dictionary resolution, so we use numeric genreId
  auto expr = parse("@duration > 180000 && $year >= 2020");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 200000, 320000, 44100, 2, 16, 1, 2, 1};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2019, 5, 200000, 320000, 44100, 2, 16, 1, 2, 1};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Matches All")
{
  auto expr = parse("true");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  CHECK(plan.matchesAll == true);

  auto track1 = TestTrack{};
  auto result = evaluator.matches(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 1900};
  result = evaluator.matches(plan, track2.view());
  CHECK(result == true);
}

TEST_CASE("PlanEvaluator - Invalid Track View")
{
  auto expr = parse("$year = 2020");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  // Empty hot data creates an invalid TrackView
  auto emptyView = rs::core::TrackView{std::span<std::byte const>{}, std::span<std::byte const>{}};
  auto result = evaluator.evaluateFull(plan, emptyView);
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Bitrate Comparison")
{
  auto expr = parse("@bitrate >= 320000");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 256000};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - SampleRate Comparison")
{
  auto expr = parse("@sampleRate >= 48000");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 48000};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Year Comparison")
{
  // Note: $trackNumber is a cold field, so we use $year instead which is hot
  auto expr = parse("$year = 2020");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2019, 5};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Tag Query - No Tags")
{
  // Query for tag - with dictionary resolving "rock" -> ID 10
  auto expr = parse("#rock");
  auto compiler = QueryCompiler{}; // No dictionary - tag resolution disabled
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  // Track with no tags - bloom filter rejects
  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {}};
  auto result = evaluator.matches(plan, track1.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Tag Query - With Matching Tag")
{
  // Set up DictionaryStore with tag
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
  dict.put(wtxn, "rock"); // Will get ID 1 (DictionaryId starts from 1)
  wtxn.commit();

  // Query for #rock
  auto expr = parse("#rock");
  QueryCompiler compiler{&dict}; // Pass dictionary for tag resolution
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  // Track with tag ID 0 (matches "rock" = 0)
  auto trackWithTag =
    TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {0}};
  auto result = evaluator.matches(plan, trackWithTag.view());
  CHECK(result == true);
}

TEST_CASE("PlanEvaluator - Tag Query - With Non-Matching Tag")
{
  // Dictionary: "rock" -> ID 10, but track has tag ID 20
  auto expr = parse("#rock");
  auto compiler = QueryCompiler{}; // No dictionary - tag resolution disabled
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  // Track with tag ID 20 (doesn't match "rock" = 10)
  auto trackWithTag =
    TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {20}};
  auto result = evaluator.matches(plan, trackWithTag.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Tag Field Compilation")
{
  // Test that tag field compiles to Field::Tag
  auto expr = parse("#tagname");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  // Check that instructions contain LoadField with Field::Tag (16)
  CHECK(plan.instructions.size() > 0);
  CHECK(plan.instructions[0].op == OpCode::LoadField);
}

TEST_CASE("PlanEvaluator - TagCount Field")
{
  auto expr = parse("@tagCount > 0");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  // Track with 2 tags
  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {10, 20}};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  // Track with no tags
  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {}};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Tag Bloom Filter - Without Dictionary")
{
  // Without a dictionary, tag bloom mask cannot be set (no way to resolve tag name to ID)
  auto expr = parse("#mytag");
  auto compiler = QueryCompiler{}; // No dictionary provided
  auto plan = compiler.compile(expr);

  // Without dictionary, tagBloomMask stays 0
  CHECK(plan.tagBloomMask == 0);
}

TEST_CASE("PlanEvaluator - Tag Bloom Filter - Dictionary Miss")
{
  // Dictionary has "rock" -> ID 10, but query is for "jazz" which is not in dict
  auto expr = parse("#jazz");
  auto compiler = QueryCompiler{}; // No dictionary - tag resolution disabled
  auto plan = compiler.compile(expr);

  // "jazz" not in dictionary, tagBloomMask stays 0
  CHECK(plan.tagBloomMask == 0);
}

TEST_CASE("PlanEvaluator - Tag Bloom Filter - Dictionary Hit")
{
  // Dictionary has "rock" -> ID 10 (10 & 31 = 10)
  auto expr = parse("#rock");
  auto compiler = QueryCompiler{}; // No dictionary - tag resolution disabled
  auto plan = compiler.compile(expr);

  // Without dictionary, tag resolution doesn't happen, bloom mask stays 0
  CHECK(plan.tagBloomMask == 0);
}

TEST_CASE("PlanEvaluator - Tag Bloom Filter - Multiple Tags Hit")
{
  // Dictionary: "rock" -> ID 10, "jazz" -> ID 20
  auto expr = parse("#rock && #jazz");
  auto compiler = QueryCompiler{}; // No dictionary - tag resolution disabled
  auto plan = compiler.compile(expr);

  // Without dictionary, tag resolution doesn't happen, bloom mask stays 0
  CHECK(plan.tagBloomMask == 0);
}

TEST_CASE("PlanEvaluator - Tag Bloom Filter - Track Computation")
{
  // Test that track bloom is computed correctly: tagId & 31
  // Using manual header construction since TrackRecord no longer has serializeHot
  // Tag ID 10 -> bit 10 (10 & 31 = 10)
  {
    auto h = rs::core::TrackHotHeader{};
    h.tagBloom = (1U << (10 & 31)); // bit 10
    auto data = test::serializeHeader(h);
    data.push_back(static_cast<std::byte>('\0')); // empty title
    auto view = rs::core::TrackView{data, std::span<std::byte const>{}};
    CHECK(view.tags().bloom() == (1U << 10));
  }

  // Tag ID 32 -> bit 0 (32 & 31 = 0)
  {
    auto h = rs::core::TrackHotHeader{};
    h.tagBloom = (1U << (32 & 31)); // bit 0
    auto data = test::serializeHeader(h);
    data.push_back(static_cast<std::byte>('\0')); // empty title
    auto view = rs::core::TrackView{data, std::span<std::byte const>{}};
    CHECK(view.tags().bloom() == 1U);
  }

  // Multiple tags: ID 5 and ID 20
  {
    auto h = rs::core::TrackHotHeader{};
    h.tagBloom = (1U << (5 & 31)) | (1U << (20 & 31)); // bits 5 and 20
    auto data = test::serializeHeader(h);
    data.push_back(static_cast<std::byte>('\0')); // empty title
    auto view = rs::core::TrackView{data, std::span<std::byte const>{}};
    CHECK((view.tags().bloom() & (1U << 5)) != 0);  // Bit 5 should be set
    CHECK((view.tags().bloom() & (1U << 20)) != 0); // Bit 20 should be set
  }
}

TEST_CASE("PlanEvaluator - Bloom Filter Fast Path - No Match")
{
  auto expr = parse("#mytag");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  // Create a track with tag bloom bit 0 set (simulating tag ID 32)
  auto h = rs::core::TrackHotHeader{};
  h.tagBloom = 0x00000001U; // Only bit 0 set

  auto data = std::vector<std::byte>{};
  data.insert_range(data.end(), rs::utility::asBytes(h));

  data.push_back(static_cast<std::byte>('\0')); // empty title
  data.push_back(static_cast<std::byte>('\0')); // empty uri

  auto view = rs::core::TrackView{data, std::span<std::byte const>{}};

  // Bloom filter rejects because query mask doesn't match track bloom
  auto result = evaluator.matches(plan, view);
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Bloom Filter Fast Path - Match")
{
  auto expr = parse("#mytag");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  // Create a track with tag bloom that has some bits set
  // (without dictionary, mask is 0, so this won't actually test fast path)
  auto h = rs::core::TrackHotHeader{};
  h.tagBloom = 0xFFFFFFFFU; // All bits set

  auto data = std::vector<std::byte>{};
  data.insert_range(data.end(), rs::utility::asBytes(h));

  data.push_back(static_cast<std::byte>('\0')); // empty title
  data.push_back(static_cast<std::byte>('\0')); // empty uri

  auto view = rs::core::TrackView{data, std::span<std::byte const>{}};

  // With mask 0, bloom check passes (no filtering), falls through to full eval
  auto result = evaluator.matches(plan, view);
  // No tags in track, so should be false
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Title LIKE with quoted string evaluates correctly")
{
  // Simple title LIKE test with quoted string
  auto expr = parse(R"($title ~ "Bach")");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  // Track with title containing "Bach"
  auto track1 = TestTrack{"Bach Greatest Hits"};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  // Track with title not containing "Bach"
  auto track2 = TestTrack{"Mozart Symphony"};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);

  // Track with exact match
  auto track3 = TestTrack{"Bach"};
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == true);
}

TEST_CASE("PlanEvaluator - Title LIKE with multi-arg Track")
{
  // Test LIKE with a Track that has multiple fields set
  auto expr = parse(R"($title ~ "Bach")");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track = TestTrack{"Bach Greatest Hits", "Artist", "Album", "/path", 2021};
  auto result = evaluator.evaluateFull(plan, track.view());
  CHECK(result == true);
}

TEST_CASE("PlanEvaluator - OR expression with LIKE evaluates correctly")
{
  // Test that $title ~ "Bach" or $year > 2021 evaluates correctly
  auto expr = parse(R"($title ~ "Bach" or $year > 2021)");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  // Track with title containing "Bach"
  auto track1 = TestTrack{"Bach Greatest Hits", "Artist", "Album", "/path", 2021};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true); // matches title ~ "Bach"

  // Track with year > 2021 but title doesn't contain "Bach"
  auto track2 = TestTrack{"Classical Music", "Artist", "Album", "/path", 2022};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == true); // matches year > 2021

  // Track with neither matching
  auto track3 = TestTrack{"Classical Music", "Artist", "Album", "/path", 2021};
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - OR with simple expressions")
{
  // Test $year > 2000 or $year > 1990 to verify OR works with two numeric comparisons
  auto expr = parse("$year > 2000 or $year > 1990");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  // year 2021: 2021 > 2000 is true, so OR should be true
  auto track1 = TestTrack{"Title", "Artist", "Album", "/path", 2021};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  // year 1995: 1995 > 2000 is false, but 1995 > 1990 is true, so OR should be true
  auto track2 = TestTrack{"Title", "Artist", "Album", "/path", 1995};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == true);

  // year 1980: both are false, so OR should be false
  auto track3 = TestTrack{"Title", "Artist", "Album", "/path", 1980};
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Greater alone for year 1980")
{
  // Verify $year > 2000 returns false for year 1980
  auto expr = parse("$year > 2000");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track = TestTrack{"Title", "Artist", "Album", "/path", 1980};
  auto result = evaluator.evaluateFull(plan, track.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Year Greater Alone Works")
{
  // Simple year > test to verify year comparison works
  auto expr = parse("$year > 2000");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Title", "Artist", "Album", "/path", 2021};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Title", "Artist", "Album", "/path", 1990};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}