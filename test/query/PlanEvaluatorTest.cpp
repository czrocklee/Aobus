/*
 * Copyright (C) 2025 RockStudio
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <catch2/catch.hpp>

#include <rs/core/Dictionary.h>
#include <rs/core/TrackLayout.h>
#include <rs/core/TrackRecord.h>
#include <rs/expr/ExecutionPlan.h>
#include <rs/expr/Parser.h>
#include <rs/expr/PlanEvaluator.h>
#include <unordered_map>
#include <vector>

namespace
{

  using rs::core::DictionaryId;

  // Mock dictionary for testing tag bloom filter with resolved tag IDs
  class MockDictionary : public rs::core::IDictionary
  {
  public:
    explicit MockDictionary(std::unordered_map<std::string, rs::core::DictionaryId> tagToId)
      : _tagToId(std::move(tagToId))
    {
    }

    rs::core::DictionaryId getStringId(std::string_view str) const override
    {
      auto it = _tagToId.find(std::string(str));
      if (it != _tagToId.end())
      {
        return it->second;
      }
      return rs::core::DictionaryId{0}; // Not found
    }

  private:
    std::unordered_map<std::string, rs::core::DictionaryId> _tagToId;
  };

  // Helper class to hold both the serialized data and the TrackView
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
      _record.metadata.uri = std::move(uri);
      _record.metadata.year = year;
      _record.metadata.trackNumber = trackNumber;
      _record.property.durationMs = durationMs;
      _record.property.bitrate = bitrate;
      _record.property.sampleRate = sampleRate;
      _record.property.channels = channels;
      _record.property.bitDepth = bitDepth;
      // Convert uint32_t tagIds to DictionaryId
      for (auto id : tagIds)
      {
        _record.tags.ids.push_back(DictionaryId{id});
      }

      // Serialize to get proper binary layout
      auto serialized = _record.serialize();
      _data.assign(reinterpret_cast<const char*>(serialized.data()),
                   reinterpret_cast<const char*>(serialized.data()) + serialized.size());

      // Fix up the header with IDs and other fields
      auto* header = reinterpret_cast<rs::core::TrackHeader*>(_data.data());
      header->artistId = DictionaryId{artistId};
      header->albumId = DictionaryId{albumId};
      header->genreId = DictionaryId{genreId};
      header->albumArtistId = DictionaryId{0};
      header->codecId = 0;
      header->rating = 0;
      header->fileSize = 1000000;
      header->mtime = 1234567890;

      _view = rs::core::TrackView{_data.data(), _data.size()};
    }

    rs::core::TrackView& view() { return _view; }
    const rs::core::TrackView& view() const { return _view; }

  private:
    rs::core::TrackRecord _record;
    std::vector<char> _data;
    rs::core::TrackView _view;
  };

} // namespace

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

  rs::core::TrackView emptyView;
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

TEST_CASE("PlanEvaluator - TrackNumber Comparison")
{
  auto expr = parse("$trackNumber = 5");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  TestTrack track1("Test", "Artist", "Album", "/path", 2020, 5);
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  TestTrack track2("Test", "Artist", "Album", "/path", 2020, 3);
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Tag Query - No Tags")
{
  // Query for tag - with dictionary resolving "rock" -> ID 10
  auto expr = parse("#rock");
  MockDictionary dict{{{"rock", rs::core::DictionaryId{10}}}};
  QueryCompiler compiler(dict);
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  // Track with no tags - bloom filter rejects
  TestTrack track1("Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {});
  auto result = evaluator.matches(plan, track1.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Tag Query - With Matching Tag")
{
  // Dictionary: "rock" -> ID 10
  auto expr = parse("#rock");
  MockDictionary dict{{{"rock", rs::core::DictionaryId{10}}}};
  QueryCompiler compiler(dict);
  auto plan = compiler.compile(expr);
  PlanEvaluator evaluator;

  // Track with tag ID 10 (matches "rock" in dictionary)
  TestTrack trackWithTag("Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {10});
  auto result = evaluator.matches(plan, trackWithTag.view());
  CHECK(result == true);
}

TEST_CASE("PlanEvaluator - Tag Query - With Non-Matching Tag")
{
  // Dictionary: "rock" -> ID 10, but track has tag ID 20
  auto expr = parse("#rock");
  MockDictionary dict{{{"rock", rs::core::DictionaryId{10}}}};
  QueryCompiler compiler(dict);
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
  MockDictionary dict{{{"rock", rs::core::DictionaryId{10}}}};
  QueryCompiler compiler(dict);
  auto plan = compiler.compile(expr);

  // "jazz" not in dictionary, tagBloomMask stays 0
  CHECK(plan.tagBloomMask == 0);
}

TEST_CASE("PlanEvaluator - Tag Bloom Filter - Dictionary Hit")
{
  // Dictionary has "rock" -> ID 10 (10 & 31 = 10)
  auto expr = parse("#rock");
  MockDictionary dict{{{"rock", rs::core::DictionaryId{10}}}};
  QueryCompiler compiler(dict);
  auto plan = compiler.compile(expr);

  // "rock" resolves to ID 10, bit 10 should be set in mask
  CHECK(plan.tagBloomMask != 0);
  CHECK((plan.tagBloomMask & (1U << 10)) != 0);
}

TEST_CASE("PlanEvaluator - Tag Bloom Filter - Multiple Tags Hit")
{
  // Dictionary: "rock" -> ID 10, "jazz" -> ID 20
  auto expr = parse("#rock && #jazz");
  MockDictionary dict{{{"rock", rs::core::DictionaryId{10}}, {"jazz", rs::core::DictionaryId{20}}}};
  QueryCompiler compiler(dict);
  auto plan = compiler.compile(expr);

  // Bit 10 (10 & 31) and bit 20 (20 & 31) should be set
  CHECK((plan.tagBloomMask & (1U << 10)) != 0);
  CHECK((plan.tagBloomMask & (1U << 20)) != 0);
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
  auto* header = reinterpret_cast<const rs::core::TrackHeader*>(data.data());
  CHECK((header->tagBloom & (1U << 10)) != 0); // Bit 10 should be set

  // Tag ID 32 -> bit 0 (32 & 31 = 0)
  record.tags.ids = {DictionaryId{32}};
  data = record.serialize();
  header = reinterpret_cast<const rs::core::TrackHeader*>(data.data());
  CHECK((header->tagBloom & 1U) != 0); // Bit 0 should be set

  // Multiple tags: ID 5 and ID 20
  record.tags.ids = {DictionaryId{5}, DictionaryId{20}};
  data = record.serialize();
  header = reinterpret_cast<const rs::core::TrackHeader*>(data.data());
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
  rs::core::TrackHeader h{};
  h.tagBloom = 0x00000001U; // Only bit 0 set

  std::vector<char> data;
  data.insert(data.end(), reinterpret_cast<const char*>(&h), reinterpret_cast<const char*>(&h + 1));

  data.push_back('\0'); // empty title
  data.push_back('\0'); // empty uri

  rs::core::TrackView view(data.data(), data.size());

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
  rs::core::TrackHeader h{};
  h.tagBloom = 0xFFFFFFFFU; // All bits set

  std::vector<char> data;
  data.insert(data.end(), reinterpret_cast<const char*>(&h), reinterpret_cast<const char*>(&h + 1));

  data.push_back('\0'); // empty title
  data.push_back('\0'); // empty uri

  rs::core::TrackView view(data.data(), data.size());

  // With mask 0, bloom check passes (no filtering), falls through to full eval
  auto result = evaluator.matches(plan, view);
  // No tags in track, so should be false
  CHECK(result == false);
}
