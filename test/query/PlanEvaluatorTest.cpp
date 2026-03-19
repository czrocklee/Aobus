// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <rs/core/DictionaryStore.h>
#include <rs/core/TrackLayout.h>
#include <rs/core/TrackRecord.h>
#include <rs/expr/ExecutionPlan.h>
#include <rs/expr/Parser.h>
#include <rs/expr/PlanEvaluator.h>
#include <rs/lmdb/Database.h>
#include <rs/lmdb/Environment.h>
#include <rs/lmdb/Transaction.h>
#include <rs/utility/ByteView.h>
#include <span>
#include <test/lmdb/LmdbTestUtils.h>

#include <vector>

namespace
{

  using rs::core::DictionaryId;

  // Helper class to hold both the serialized data and the TrackHotView
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
      _record.metadata.title = std::move(title);
      _record.metadata.artist = std::move(artist);
      _record.metadata.album = std::move(album);
      _record.metadata.uri = std::move(uri);
      _record.metadata.year = year;
      _record.metadata.trackNumber = trackNumber;
      _record.property.durationMs = durationMs;
      _record.property.bitrate = bitrate;
      _record.property.sampleRate = sampleRate;
      _record.property.channels = channels;
      _record.property.bitDepth = bitDepth;
      // Convert uint32_t tagIds to DictionaryId
      for (auto id : tagIds) { _record.tags.ids.push_back(DictionaryId{id}); }

      // Serialize hot data to get proper binary layout
      _hotData = _record.serializeHot();
      _hotView = rs::core::TrackHotView{std::span<std::byte const>{_hotData.data(), _hotData.size()}};

      // Fix up the header with IDs and other fields
      auto* header = const_cast<rs::core::TrackHotHeader*>(_hotView.header());
      header->artistId = DictionaryId{artistId};
      header->albumId = DictionaryId{albumId};
      header->genreId = DictionaryId{genreId};
      header->albumArtistId = DictionaryId{0};
      header->codecId = 0;
      header->rating = 0;
      header->fileSize = 1000000;
      header->mtime = 1234567890;
    }

    rs::core::TrackHotView& view() { return _hotView; }
    rs::core::TrackHotView const& view() const { return _hotView; }

  private:
    rs::core::TrackRecord _record;
    std::vector<std::byte> _hotData;
    rs::core::TrackHotView _hotView;
  };

} // namespace

using rs::core::DictionaryStore;
using rs::core::DictionaryId;
using rs::lmdb::Database;
using rs::lmdb::Environment;
using rs::lmdb::ReadTransaction;
using rs::lmdb::WriteTransaction;
using namespace rs::expr;

TEST_CASE("PlanEvaluator - Simple Equal Match")
{
  auto expr = parse("$year = 2020");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  TestTrack track1("Test", "Artist", "Album", "/path", 2020);
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  TestTrack track2("Test", "Artist", "Album", "/path", 2019);
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Greater Than")
{
  auto expr = parse("@duration > 179000");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  TestTrack track1("Test", "Artist", "Album", "/path", 2020, 5, 180000);
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  TestTrack track2("Test", "Artist", "Album", "/path", 2020, 5, 170000);
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - NotEqual")
{
  auto expr = parse("$year != 2020");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  TestTrack track1("Test", "Artist", "Album", "/path", 2020, 5, 180000);
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == false);

  TestTrack track2("Test", "Artist", "Album", "/path", 2021, 5, 180000);
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == true);

  TestTrack track3("Test", "Artist", "Album", "/path", 2019, 5, 180000);
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == true);
}

TEST_CASE("PlanEvaluator - Greater Than Or Equal")
{
  auto expr = parse("@duration >= 180000");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  TestTrack track1("Test", "Artist", "Album", "/path", 2020, 5, 180000);
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  TestTrack track2("Test", "Artist", "Album", "/path", 2020, 5, 179999);
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Less Than")
{
  auto expr = parse("$year < 2021");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  TestTrack track1("Test", "Artist", "Album", "/path", 2020);
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  TestTrack track2("Test", "Artist", "Album", "/path", 2021);
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Like Match")
{
  auto expr = parse("$title ~ Test");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  TestTrack track1("Test Title");
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  TestTrack track2("Another Title");
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Title String Equal")
{
  auto expr = parse("$title = 'Hello World'");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  TestTrack track1("Hello World");
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  TestTrack track2("hello world"); // case-sensitive
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);

  TestTrack track3("Hello");
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Title String Not Equal")
{
  auto expr = parse("$title != 'Hello'");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  TestTrack track1("Hello World");
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  TestTrack track2("Hello");
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Title String Less Than")
{
  auto expr = parse("$title < 'zoo'");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  TestTrack track1("apple");
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  TestTrack track2("zoo");
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);

  TestTrack track3("zooExtra");
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Title String Greater Than")
{
  auto expr = parse("$title > 'apple'");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  TestTrack track1("banana");
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  TestTrack track2("apple");
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);

  TestTrack track3("Apple"); // case-sensitive
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Logical And")
{
  auto expr = parse("$year = 2020 && @duration > 100000");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  TestTrack track1("Test", "Artist", "Album", "/path", 2020, 5, 180000);
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  TestTrack track2("Test", "Artist", "Album", "/path", 2019, 5, 180000);
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);

  TestTrack track3("Test", "Artist", "Album", "/path", 2020, 5, 50000);
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Logical Or")
{
  auto expr = parse("$year = 2020 || $year = 2019");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  TestTrack track1("Test", "Artist", "Album", "/path", 2020);
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  TestTrack track2("Test", "Artist", "Album", "/path", 2019);
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == true);

  TestTrack track3("Test", "Artist", "Album", "/path", 2018);
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Logical Not")
{
  // Use "not(" for explicit grouping, or check if parser handles precedence
  auto expr = parse("!($year = 2020)");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  TestTrack track1("Test", "Artist", "Album", "/path", 2020);
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == false);

  TestTrack track2("Test", "Artist", "Album", "/path", 2019);
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == true);
}

TEST_CASE("PlanEvaluator - Complex Expression")
{
  // Note: genre comparison requires dictionary resolution, so we use numeric genreId
  auto expr = parse("@duration > 180000 && $year >= 2020");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  TestTrack track1("Test", "Artist", "Album", "/path", 2020, 5, 200000, 320000, 44100, 2, 16, 1, 2, 1);
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  TestTrack track2("Test", "Artist", "Album", "/path", 2019, 5, 200000, 320000, 44100, 2, 16, 1, 2, 1);
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Matches All")
{
  auto expr = parse("true");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  CHECK(plan.matchesAll == true);

  TestTrack track1;
  auto result = evaluator.matches(plan, track1.view());
  CHECK(result == true);

  TestTrack track2("Test", "Artist", "Album", "/path", 1900);
  result = evaluator.matches(plan, track2.view());
  CHECK(result == true);
}

TEST_CASE("PlanEvaluator - Invalid Track View")
{
  auto expr = parse("$year = 2020");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  rs::core::TrackHotView emptyView;
  auto result = evaluator.evaluateFull(plan, emptyView);
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Bitrate Comparison")
{
  auto expr = parse("@bitrate >= 320000");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  TestTrack track1("Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000);
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  TestTrack track2("Test", "Artist", "Album", "/path", 2020, 5, 180000, 256000);
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - SampleRate Comparison")
{
  auto expr = parse("@sampleRate >= 48000");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  TestTrack track1("Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 48000);
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  TestTrack track2("Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100);
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Year Comparison")
{
  // Note: $trackNumber is a cold field, so we use $year instead which is hot
  auto expr = parse("$year = 2020");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  TestTrack track1("Test", "Artist", "Album", "/path", 2020, 5);
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  TestTrack track2("Test", "Artist", "Album", "/path", 2019, 5);
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Tag Query - No Tags")
{
  // Query for tag - with dictionary resolving "rock" -> ID 10
  auto expr = parse("#rock");
  QueryCompiler compiler; // No dictionary - tag resolution disabled
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  // Track with no tags - bloom filter rejects
  TestTrack track1("Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {});
  auto result = evaluator.matches(plan, track1.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Tag Query - With Matching Tag")
{
  // Set up DictionaryStore with tag
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  DictionaryStore dict{wtxn, "dict"};
  dict.put(wtxn, "rock"); // Will get ID 0 (LMDB starts from 0 now)
  wtxn.commit();

  // Query for #rock
  auto expr = parse("#rock");
  QueryCompiler compiler{&dict}; // Pass dictionary for tag resolution
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  // Track with tag ID 0 (matches "rock" = 0)
  TestTrack trackWithTag("Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {0});
  auto result = evaluator.matches(plan, trackWithTag.view());
  CHECK(result == true);
}

TEST_CASE("PlanEvaluator - Tag Query - With Non-Matching Tag")
{
  // Dictionary: "rock" -> ID 10, but track has tag ID 20
  auto expr = parse("#rock");
  QueryCompiler compiler; // No dictionary - tag resolution disabled
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  // Track with tag ID 20 (doesn't match "rock" = 10)
  TestTrack trackWithTag("Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {20});
  auto result = evaluator.matches(plan, trackWithTag.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Tag Field Compilation")
{
  // Test that tag field compiles to Field::Tag
  auto expr = parse("#tagname");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);

  // Check that instructions contain LoadField with Field::Tag (16)
  CHECK(plan.instructions.size() > 0);
  CHECK(plan.instructions[0].op == OpCode::LoadField);
}

TEST_CASE("PlanEvaluator - TagCount Field")
{
  auto expr = parse("@tagCount > 0");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  // Track with 2 tags
  TestTrack track1("Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {10, 20});
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  // Track with no tags
  TestTrack track2("Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {});
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Tag Bloom Filter - Without Dictionary")
{
  // Without a dictionary, tag bloom mask cannot be set (no way to resolve tag name to ID)
  auto expr = parse("#mytag");
  QueryCompiler compiler; // No dictionary provided
  auto plan = compiler.compile(expr);

  // Without dictionary, tagBloomMask stays 0
  CHECK(plan.tagBloomMask == 0);
}

TEST_CASE("PlanEvaluator - Tag Bloom Filter - Dictionary Miss")
{
  // Dictionary has "rock" -> ID 10, but query is for "jazz" which is not in dict
  auto expr = parse("#jazz");
  QueryCompiler compiler; // No dictionary - tag resolution disabled
  auto plan = compiler.compile(expr);

  // "jazz" not in dictionary, tagBloomMask stays 0
  CHECK(plan.tagBloomMask == 0);
}

TEST_CASE("PlanEvaluator - Tag Bloom Filter - Dictionary Hit")
{
  // Dictionary has "rock" -> ID 10 (10 & 31 = 10)
  auto expr = parse("#rock");
  QueryCompiler compiler; // No dictionary - tag resolution disabled
  auto plan = compiler.compile(expr);

  // Without dictionary, tag resolution doesn't happen, bloom mask stays 0
  CHECK(plan.tagBloomMask == 0);
}

TEST_CASE("PlanEvaluator - Tag Bloom Filter - Multiple Tags Hit")
{
  // Dictionary: "rock" -> ID 10, "jazz" -> ID 20
  auto expr = parse("#rock && #jazz");
  QueryCompiler compiler; // No dictionary - tag resolution disabled
  auto plan = compiler.compile(expr);

  // Without dictionary, tag resolution doesn't happen, bloom mask stays 0
  CHECK(plan.tagBloomMask == 0);
}

TEST_CASE("PlanEvaluator - Tag Bloom Filter - Track Computation")
{
  // Test that track bloom is computed correctly: tagId & 31
  rs::core::TrackRecord record;
  record.metadata.title = "Test";
  record.metadata.uri = "/test";

  // Tag ID 10 -> bit 10 (10 & 31 = 10)
  record.tags.ids = {DictionaryId{10}};
  auto data = record.serialize();
  auto* header = rs::utility::as<rs::core::TrackHeader const>(data);
  CHECK((header->tagBloom & (1U << 10)) != 0); // Bit 10 should be set

  // Tag ID 32 -> bit 0 (32 & 31 = 0)
  record.tags.ids = {DictionaryId{32}};
  data = record.serialize();
  header = rs::utility::as<rs::core::TrackHeader const>(data);
  CHECK((header->tagBloom & 1U) != 0); // Bit 0 should be set

  // Multiple tags: ID 5 and ID 20
  record.tags.ids = {DictionaryId{5}, DictionaryId{20}};
  data = record.serialize();
  header = rs::utility::as<rs::core::TrackHeader const>(data);
  CHECK((header->tagBloom & (1U << 5)) != 0);  // Bit 5 should be set
  CHECK((header->tagBloom & (1U << 20)) != 0); // Bit 20 should be set
}

TEST_CASE("PlanEvaluator - Bloom Filter Fast Path - No Match")
{
  auto expr = parse("#mytag");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  // Create a track with tag bloom bit 0 set (simulating tag ID 32)
  rs::core::TrackHotHeader h{};
  h.tagBloom = 0x00000001U; // Only bit 0 set

  std::vector<std::byte> data;
  data.insert_range(data.end(), rs::utility::asBytes(h));

  data.push_back(static_cast<std::byte>('\0')); // empty title
  data.push_back(static_cast<std::byte>('\0')); // empty uri

  rs::core::TrackHotView view(std::as_bytes(std::span{data}));

  // Bloom filter rejects because query mask doesn't match track bloom
  auto result = evaluator.matches(plan, view);
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Bloom Filter Fast Path - Match")
{
  auto expr = parse("#mytag");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  // Create a track with tag bloom that has some bits set
  // (without dictionary, mask is 0, so this won't actually test fast path)
  rs::core::TrackHotHeader h{};
  h.tagBloom = 0xFFFFFFFFU; // All bits set

  std::vector<std::byte> data;
  data.insert_range(data.end(), rs::utility::asBytes(h));

  data.push_back(static_cast<std::byte>('\0')); // empty title
  data.push_back(static_cast<std::byte>('\0')); // empty uri

  rs::core::TrackHotView view(std::as_bytes(std::span{data}));

  // With mask 0, bloom check passes (no filtering), falls through to full eval
  auto result = evaluator.matches(plan, view);
  // No tags in track, so should be false
  CHECK(result == false);
}
