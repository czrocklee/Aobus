// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <ao/library/DictionaryStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/Parser.h>

#include <test/unit/lmdb/TestUtils.h>

using namespace ao::query;

TEST_CASE("ExecutionPlan - Compile Simple Expression")
{
  auto expr = parse("$artist = Bach");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK_FALSE(plan.instructions.empty());
  CHECK_FALSE(plan.matchesAll);
}

TEST_CASE("ExecutionPlan - Compile Empty Expression")
{
  // Note: matchesAll is not automatically set - it's a hint for optimization
  // The plan should still compile a constant true expression
  auto expr = parse("true");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  // The plan should have at least one instruction (LoadConstant)
  CHECK(plan.instructions.size() >= 1);
}

TEST_CASE("ExecutionPlan - Compile Metadata Field")
{
  auto expr = parse("$title = 'Test'");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK(plan.instructions.size() >= 2);
  CHECK(plan.instructions[0].op == OpCode::LoadField);

  bool hasEq = false;
  for (auto const& instr : plan.instructions)
  {
    if (instr.op == OpCode::Eq)
    {
      hasEq = true;
      break;
    }
  }
  CHECK(hasEq == true);
}

TEST_CASE("ExecutionPlan - Compile Property Field")
{
  auto expr = parse("@duration > 180000");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK(plan.instructions.size() >= 2);
  CHECK(plan.instructions[0].op == OpCode::LoadField);

  bool hasGt = false;
  for (auto const& instr : plan.instructions)
  {
    if (instr.op == OpCode::Gt)
    {
      hasGt = true;
      break;
    }
  }
  CHECK(hasGt == true);
}

TEST_CASE("ExecutionPlan - Property Alias Maps To Bitrate Field")
{
  auto expr = parse("@br >= 320k");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  REQUIRE_FALSE(plan.instructions.empty());
  CHECK(plan.instructions[0].op == OpCode::LoadField);
  CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(Field::Bitrate));
}

TEST_CASE("ExecutionPlan - Metadata Alias Maps To AlbumArtist Field")
{
  auto expr = parse("$aa = Bach");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  REQUIRE_FALSE(plan.instructions.empty());
  CHECK(plan.instructions[0].op == OpCode::LoadField);
  CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(Field::AlbumArtistId));
}

TEST_CASE("ExecutionPlan - Duration Unit Constant")
{
  auto expr = parse("@duration >= 3m");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  auto const it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);

  REQUIRE(it != plan.instructions.end());
  CHECK(it->constValue == 180000);
}

TEST_CASE("ExecutionPlan - Bitrate Unit Constant")
{
  auto expr = parse("@bitrate >= 2m");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  auto const it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);

  REQUIRE(it != plan.instructions.end());
  CHECK(it->constValue == 2000000);
}

TEST_CASE("ExecutionPlan - SampleRate Unit Constant")
{
  auto expr = parse("@sampleRate = 44.1k");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  auto const it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);

  REQUIRE(it != plan.instructions.end());
  CHECK(it->constValue == 44100);
}

TEST_CASE("ExecutionPlan - Unit Constant Rejects Unsupported Field")
{
  auto expr = parse("$year >= 3m");
  auto compiler = QueryCompiler{};

  REQUIRE_THROWS(compiler.compile(expr));
}

TEST_CASE("ExecutionPlan - Compile Logical And")
{
  // Use && for logical and to ensure it's parsed correctly
  auto expr = parse("$artist = Bach && $genre = Classical");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  bool hasAnd = false;
  for (auto const& instr : plan.instructions)
  {
    if (instr.op == OpCode::And)
    {
      hasAnd = true;
      break;
    }
  }
  CHECK(hasAnd == true);
}

TEST_CASE("ExecutionPlan - Compile Logical Or")
{
  // Use || for logical or to ensure it's parsed correctly
  auto expr = parse("$artist = Bach || $artist = Mozart");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  bool hasOr = false;
  for (auto const& instr : plan.instructions)
  {
    if (instr.op == OpCode::Or)
    {
      hasOr = true;
      break;
    }
  }
  CHECK(hasOr == true);
}

TEST_CASE("ExecutionPlan - Compile Logical Not")
{
  auto expr = parse("not $artist");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  bool hasNot = false;
  for (auto const& instr : plan.instructions)
  {
    if (instr.op == OpCode::Not)
    {
      hasNot = true;
      break;
    }
  }
  CHECK(hasNot == true);
}

TEST_CASE("ExecutionPlan - Compile Relational Operators")
{
  auto expr = parse("$year < 2000");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  bool hasLt = false;
  for (auto const& instr : plan.instructions)
  {
    if (instr.op == OpCode::Lt)
    {
      hasLt = true;
      break;
    }
  }
  CHECK(hasLt == true);

  expr = parse("$year <= 2000");
  compiler = QueryCompiler();
  plan = compiler.compile(expr);

  bool hasLe = false;
  for (auto const& instr : plan.instructions)
  {
    if (instr.op == OpCode::Le)
    {
      hasLe = true;
      break;
    }
  }
  CHECK(hasLe == true);
}

TEST_CASE("ExecutionPlan - Compile Like")
{
  auto expr = parse("$title ~ Love");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  bool hasLike = false;
  for (auto const& instr : plan.instructions)
  {
    if (instr.op == OpCode::Like)
    {
      hasLike = true;
      break;
    }
  }
  CHECK(hasLike == true);
}

TEST_CASE("ExecutionPlan - Compile String Constant")
{
  auto expr = parse("$title = 'Hello World'");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK_FALSE(plan.stringConstants.empty());
  CHECK(plan.stringConstants[0] == "Hello World");
}

TEST_CASE("ExecutionPlan - Field Enum Values")
{
  // String fields
  CHECK(static_cast<std::uint8_t>(Field::Title) == 0);
  CHECK(static_cast<std::uint8_t>(Field::Uri) == 1);

  // Property fields
  CHECK(static_cast<std::uint8_t>(Field::DurationMs) == 2);
  CHECK(static_cast<std::uint8_t>(Field::Bitrate) == 3);
  CHECK(static_cast<std::uint8_t>(Field::SampleRate) == 4);

  // Metadata ID fields
  CHECK(static_cast<std::uint8_t>(Field::ArtistId) == 9);
  CHECK(static_cast<std::uint8_t>(Field::AlbumId) == 10);
  CHECK(static_cast<std::uint8_t>(Field::ComposerId) == 13);
  CHECK(static_cast<std::uint8_t>(Field::WorkId) == 15);

  // Metadata numeric fields
  CHECK(static_cast<std::uint8_t>(Field::Year) == 16);
  CHECK(static_cast<std::uint8_t>(Field::TrackNumber) == 17);

  // Tag fields
  CHECK(static_cast<std::uint8_t>(Field::TagBloom) == 21);
  CHECK(static_cast<std::uint8_t>(Field::TagCount) == 22);
}

TEST_CASE("ExecutionPlan - OpCode Enum Values")
{
  CHECK(static_cast<std::uint8_t>(OpCode::Nop) == 0);
  CHECK(static_cast<std::uint8_t>(OpCode::LoadField) == 1);
  CHECK(static_cast<std::uint8_t>(OpCode::LoadConstant) == 2);
  CHECK(static_cast<std::uint8_t>(OpCode::Eq) == 3);
  CHECK(static_cast<std::uint8_t>(OpCode::Ne) == 4);
}

TEST_CASE("ExecutionPlan - AccessProfile HotOnly")
{
  // Metadata variable -> HotOnly
  auto expr = parse("$artist = Bach");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK(plan.accessProfile == AccessProfile::HotOnly);
}

TEST_CASE("ExecutionPlan - AccessProfile ColdOnly")
{
  // Custom variable -> ColdOnly
  auto expr = parse("%customkey = value");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK(plan.accessProfile == AccessProfile::ColdOnly);
}

TEST_CASE("ExecutionPlan - AccessProfile HotAndCold")
{
  // Mix of hot and cold -> HotAndCold
  auto expr = parse("$artist = Bach && %customkey = value");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK(plan.accessProfile == AccessProfile::HotAndCold);
}

TEST_CASE("ExecutionPlan - AccessProfile Property Field")
{
  // Property variable -> ColdOnly (stored in TrackColdHeader)
  auto expr = parse("@duration > 180000");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK(plan.accessProfile == AccessProfile::ColdOnly);
}

TEST_CASE("ExecutionPlan - AccessProfile Tag Field")
{
  // Tag variable -> HotOnly
  auto expr = parse("#rock");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK(plan.accessProfile == AccessProfile::HotOnly);
}

TEST_CASE("ExecutionPlan - AccessProfile Cold Field")
{
  // TrackNumber field is in cold storage -> ColdOnly
  auto expr = parse("$trackNumber > 5");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK(plan.accessProfile == AccessProfile::ColdOnly);
}

TEST_CASE("ExecutionPlan - AccessProfile DurationMs is ColdOnly")
{
  auto expr = parse("@duration > 180000");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  CHECK(plan.accessProfile == AccessProfile::ColdOnly);
}

TEST_CASE("ExecutionPlan - AccessProfile Bitrate is ColdOnly")
{
  auto expr = parse("@bitrate > 320");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  CHECK(plan.accessProfile == AccessProfile::ColdOnly);
}

TEST_CASE("ExecutionPlan - AccessProfile SampleRate is ColdOnly")
{
  auto expr = parse("@sampleRate = 44100");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  CHECK(plan.accessProfile == AccessProfile::ColdOnly);
}

TEST_CASE("ExecutionPlan - AccessProfile Channels is ColdOnly")
{
  auto expr = parse("@channels = 2");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  CHECK(plan.accessProfile == AccessProfile::ColdOnly);
}

TEST_CASE("ExecutionPlan - AccessProfile Mixed HotAndCold")
{
  // Mix of hot ($year) and cold ($trackNumber) -> HotAndCold
  auto expr = parse("$year > 2020 && $trackNumber > 5");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK(plan.accessProfile == AccessProfile::HotAndCold);
}

TEST_CASE("ExecutionPlan - AccessProfile Custom Field")
{
  // Custom variable -> ColdOnly
  auto expr = parse("%customkey = value");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK(plan.accessProfile == AccessProfile::ColdOnly);
}

TEST_CASE("ExecutionPlan - LIKE operator works for ArtistId")
{
  auto temp = TempDir{};
  auto env = ao::lmdb::Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
  auto wtxn = ao::lmdb::WriteTransaction{env};
  auto dict = ao::library::DictionaryStore{ao::lmdb::Database{wtxn, "dict"}, wtxn};
  dict.put(wtxn, "Johann Sebastian Bach");

  auto expr = parse(R"($artist ~ "Bach")");
  auto compiler = QueryCompiler{&dict};
  auto plan = compiler.compile(expr);

  REQUIRE(plan.dictionary == &dict);
  REQUIRE(plan.stringConstants.size() == 1);
  CHECK(plan.stringConstants.front() == "Bach");
  CHECK(plan.instructions.back().op == OpCode::Like);
  CHECK(plan.instructions.back().field == static_cast<std::uint8_t>(Field::ArtistId));
}

TEST_CASE("ExecutionPlan - LIKE operator works for AlbumId")
{
  auto expr = parse(R"($album ~ "Greatest Hits")");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK_FALSE(plan.instructions.empty());
}

TEST_CASE("ExecutionPlan - LIKE operator works for GenreId")
{
  auto expr = parse(R"($genre ~ "Rock")");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK_FALSE(plan.instructions.empty());
}

TEST_CASE("ExecutionPlan - LIKE operator works for AlbumArtistId")
{
  auto expr = parse(R"($albumArtist ~ "Bach")");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK_FALSE(plan.instructions.empty());
}

TEST_CASE("ExecutionPlan - LIKE operator not supported for CoverArtId")
{
  auto expr = parse(R"($coverArt ~ "front")");
  auto compiler = QueryCompiler{};
  REQUIRE_THROWS(compiler.compile(expr));
}

TEST_CASE("ExecutionPlan - LIKE operator not supported for Tags")
{
  auto expr = parse(R"(#rock ~ "progressive")");
  auto compiler = QueryCompiler{};
  REQUIRE_THROWS(compiler.compile(expr));
}

TEST_CASE("ExecutionPlan - LIKE operator works for Title")
{
  auto expr = parse(R"($title ~ "Bach")");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK_FALSE(plan.instructions.empty());
  CHECK_FALSE(plan.matchesAll);
}

TEST_CASE("ExecutionPlan - Unknown Metadata Field Throws")
{
  auto expr = parse("$uri = 'x'");
  auto compiler = QueryCompiler{};
  REQUIRE_THROWS(compiler.compile(expr));
}

TEST_CASE("ExecutionPlan - Unknown Property Field Throws")
{
  auto expr = parse("@tagCount > 0");
  auto compiler = QueryCompiler{};
  REQUIRE_THROWS(compiler.compile(expr));
}

TEST_CASE("ExecutionPlan - Add Operator Is Rejected")
{
  auto expr = parse("$title + $artist");
  auto compiler = QueryCompiler{};
  REQUIRE_THROWS(compiler.compile(expr));
}

TEST_CASE("ExecutionPlan - Mixed LIKE and EQUAL in OR expression")
{
  // This tests that leftField is correctly saved before compiling right operand
  // $title ~ "Bach" should NOT check if ArtistId is used with LIKE
  auto expr = parse(R"($title ~ "Bach" or $artist = "Bach")");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK_FALSE(plan.instructions.empty());
  CHECK_FALSE(plan.matchesAll);
}

TEST_CASE("ExecutionPlan - Parenthesized LIKE and EQUAL in OR expression")
{
  // Explicit grouping with parentheses should also work
  auto expr = parse(R"(($title ~ "Bach") or ($artist = "Bach"))");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK_FALSE(plan.instructions.empty());
  CHECK_FALSE(plan.matchesAll);
}

TEST_CASE("ExecutionPlan - Multiple OR with ID field equality")
{
  // Multiple ID field equalities in OR should compile without throwing
  auto expr = parse(R"($artist = "Bach" or $artist = "Mozart" or $album = "交响乐")");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK_FALSE(plan.instructions.empty());
  CHECK_FALSE(plan.matchesAll);
}

TEST_CASE("ExecutionPlan - Title LIKE chained with AND")
{
  // Title LIKE should work with AND
  auto expr = parse(R"($title ~ "Bach" and $year > 2000)");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK_FALSE(plan.instructions.empty());
  CHECK_FALSE(plan.matchesAll);
}

TEST_CASE("ExecutionPlan - Future matching for tags not yet in dictionary")
{
  auto temp = TempDir{};
  auto env = ao::lmdb::Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
  auto wtxn = ao::lmdb::WriteTransaction{env};
  auto dict = ao::library::DictionaryStore{ao::lmdb::Database{wtxn, "dict"}, wtxn};

  // Tag "FutureTag" does not exist in dictionary yet
  auto expr = parse("#FutureTag");
  auto compiler = QueryCompiler{&dict};

  // Compile the plan. This should use getOrIntern() to allocate a stable ID
  auto plan = compiler.compile(expr);

  // The ID should now be in the dictionary because of getOrIntern()
  CHECK(dict.contains("FutureTag"));
  auto futureTagId = dict.getId("FutureTag");

  // Verify that the instruction uses this ID
  bool foundTagEq = false;
  for (auto const& instr : plan.instructions)
  {
    if (instr.op == OpCode::Eq)
    {
      // The register before Eq should contain the constant we loaded
      auto const& loadInstr = plan.instructions[&instr - &plan.instructions[0] - 1];
      if (loadInstr.op == OpCode::LoadConstant)
      {
        CHECK(loadInstr.constValue == static_cast<std::int64_t>(futureTagId.value()));
        foundTagEq = true;
      }
    }
  }
  CHECK(foundTagEq);

  // Verify Bloom Filter also contains the bit for this getOrInternd ID
  std::uint32_t expectedBit = std::uint32_t{1} << (futureTagId.value() & 31); // 31 is kBloomBitMask
  CHECK((plan.tagBloomMask & expectedBit) == expectedBit);
}

TEST_CASE("ExecutionPlan - Future matching for custom fields not yet in dictionary")
{
  auto temp = TempDir{};
  auto env = ao::lmdb::Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
  auto wtxn = ao::lmdb::WriteTransaction{env};
  auto dict = ao::library::DictionaryStore{ao::lmdb::Database{wtxn, "dict"}, wtxn};

  // Custom field "FutureKey" does not exist
  auto expr = parse("%FutureKey = 'Value'");
  auto compiler = QueryCompiler{&dict};

  auto plan = compiler.compile(expr);

  // ID should have been getOrInternd
  CHECK(dict.contains("FutureKey"));
  auto futureKeyId = dict.getId("FutureKey");

  // Verify that the instruction uses this ID
  bool foundLoadField = false;
  for (auto const& instr : plan.instructions)
  {
    if (instr.op == OpCode::LoadField && instr.field == static_cast<std::uint8_t>(Field::Custom))
    {
      CHECK(instr.constValue == static_cast<std::int64_t>(futureKeyId.value()));
      foundLoadField = true;
    }
  }
  CHECK(foundLoadField);
}
TEST_CASE("ExecutionPlan - Metadata Dispatch Maps Every Supported Name")
{
  struct Case
  {
    std::string name;
    Field expected;
  };
  auto cases = {Case{"year", Field::Year},
                Case{"y", Field::Year},
                Case{"trackNumber", Field::TrackNumber},
                Case{"tn", Field::TrackNumber},
                Case{"totalTracks", Field::TotalTracks},
                Case{"tt", Field::TotalTracks},
                Case{"discNumber", Field::DiscNumber},
                Case{"dn", Field::DiscNumber},
                Case{"totalDiscs", Field::TotalDiscs},
                Case{"td", Field::TotalDiscs},
                Case{"artist", Field::ArtistId},
                Case{"a", Field::ArtistId},
                Case{"album", Field::AlbumId},
                Case{"al", Field::AlbumId},
                Case{"genre", Field::GenreId},
                Case{"g", Field::GenreId},
                Case{"composer", Field::ComposerId},
                Case{"c", Field::ComposerId},
                Case{"albumArtist", Field::AlbumArtistId},
                Case{"aa", Field::AlbumArtistId},
                Case{"coverArt", Field::CoverArtId},
                Case{"ca", Field::CoverArtId},
                Case{"title", Field::Title},
                Case{"t", Field::Title},
                Case{"work", Field::WorkId},
                Case{"w", Field::WorkId}};

  for (auto const& c : cases)
  {
    DYNAMIC_SECTION("Field: " << c.name)
    {
      auto expr = parse("$" + c.name + " = 'x'");
      auto compiler = QueryCompiler{};
      auto plan = compiler.compile(expr);
      REQUIRE_FALSE(plan.instructions.empty());
      CHECK(plan.instructions[0].op == OpCode::LoadField);
      CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(c.expected));
    }
  }
}

TEST_CASE("ExecutionPlan - Property Dispatch Maps Every Supported Name")
{
  struct Case
  {
    std::string name;
    Field expected;
  };
  auto cases = {Case{"duration", Field::DurationMs},
                Case{"l", Field::DurationMs},
                Case{"bitrate", Field::Bitrate},
                Case{"br", Field::Bitrate},
                Case{"sampleRate", Field::SampleRate},
                Case{"sr", Field::SampleRate},
                Case{"channels", Field::Channels},
                Case{"bitDepth", Field::BitDepth},
                Case{"bd", Field::BitDepth}};

  for (auto const& c : cases)
  {
    DYNAMIC_SECTION("Field: " << c.name)
    {
      auto expr = parse("@" + c.name + " >= 0");
      auto compiler = QueryCompiler{};
      auto plan = compiler.compile(expr);
      REQUIRE_FALSE(plan.instructions.empty());
      CHECK(plan.instructions[0].op == OpCode::LoadField);
      CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(c.expected));
    }
  }
}

TEST_CASE("ExecutionPlan - AccessProfile Exhaustive Classification")
{
  auto compiler = QueryCompiler{};

  SECTION("HotOnly")
  {
    auto fields = {"$title",
                   "$artist",
                   "$album",
                   "$genre",
                   "$albumArtist",
                   "$composer",
                   "$year",
                   "#rock",
                   "true",
                   "false",
                   "@bitDepth",
                   "@rating",
                   "@codecId"};
    for (auto const* f : fields)
    {
      auto expr = parse(f);
      if (f[0] != '#' && std::string(f) != "true" && std::string(f) != "false")
      {
        expr = parse(std::string(f) + " = 0");
      }
      auto plan = compiler.compile(expr);
      CHECK(plan.accessProfile == AccessProfile::HotOnly);
    }
  }

  SECTION("ColdOnly")
  {
    auto fields = {"$trackNumber",
                   "$totalTracks",
                   "$discNumber",
                   "$totalDiscs",
                   "$coverArt",
                   "$work",
                   "%isrc",
                   "@duration",
                   "@bitrate",
                   "@sampleRate",
                   "@channels"};
    for (auto const* f : fields)
    {
      auto expr = parse(std::string(f) + " >= 0");
      auto plan = compiler.compile(expr);
      CHECK(plan.accessProfile == AccessProfile::ColdOnly);
    }
  }

  SECTION("HotAndCold")
  {
    auto expr = parse("$year >= 2020 and @duration >= 3m");
    auto plan = compiler.compile(expr);
    CHECK(plan.accessProfile == AccessProfile::HotAndCold);
  }
}
TEST_CASE("ExecutionPlan - Boolean False Compiles To ConstantZero")
{
  auto expr = parse("false");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  REQUIRE(plan.instructions.size() >= 1);
  CHECK(plan.instructions[0].op == OpCode::LoadConstant);
  CHECK(plan.instructions[0].constValue == 0);
  CHECK_FALSE(plan.matchesAll);
}

TEST_CASE("ExecutionPlan - String Constant Deduplication")
{
  SECTION("Reuses Identical String Constants")
  {
    auto expr = parse("$title = \"Bach\" or $title != \"Bach\"");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);
    CHECK(plan.stringConstants.size() == 1);
    CHECK(plan.stringConstants[0] == "Bach");
  }

  SECTION("Stores Different String Constants Separately")
  {
    auto expr = parse("$title = \"Bach\" or $title = \"Mozart\"");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);
    CHECK(plan.stringConstants.size() == 2);
  }
}

TEST_CASE("ExecutionPlan - Unit Literal Scaling")
{
  auto compiler = QueryCompiler{};

  SECTION("Duration Supports MsSMMHUnits")
  {
    struct Case
    {
      std::string unit;
      std::int64_t expected;
    };
    auto cases = {Case{"1ms", 1}, Case{"1s", 1000}, Case{"1m", 60000}, Case{"1h", 3600000}};
    for (auto const& c : cases)
    {
      auto expr = parse("@duration >= " + c.unit);
      auto plan = compiler.compile(expr);
      auto const it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);
      CHECK(it->constValue == c.expected);
    }
  }

  SECTION("Bitrate and SampleRate Support KAndMUnits")
  {
    auto expr1 = parse("@bitrate >= 256k");
    CHECK(compiler.compile(expr1).instructions[1].constValue == 256000);

    auto expr2 = parse("@sampleRate >= 44.1k");
    CHECK(compiler.compile(expr2).instructions[1].constValue == 44100);
  }

  SECTION("Unit Suffix Is CaseInsensitive")
  {
    auto expr = parse("@bitrate >= 256K");
    CHECK(compiler.compile(expr).instructions[1].constValue == 256000);
  }

  SECTION("Negative Unit Literal Compiles")
  {
    auto expr = parse("@bitrate >= -2k");
    CHECK(compiler.compile(expr).instructions[1].constValue == -2000);
  }
}

TEST_CASE("ExecutionPlan - Unit Literal Error Paths")
{
  auto compiler = QueryCompiler{};

  SECTION("Rejects UnsupportedSuffixForField")
  {
    REQUIRE_THROWS(compiler.compile(parse("@duration >= 10k")));
    REQUIRE_THROWS(compiler.compile(parse("@bitrate >= 3h")));
  }

  SECTION("Rejects MissingNumericFieldContext")
  {
    // A top-level unit constant expression like "3m" should fail
    REQUIRE_THROWS(compiler.compile(parse("3m")));
  }
}

TEST_CASE("ExecutionPlan - Tag Bloom Mask Compilation")
{
  auto temp = TempDir{};
  auto env = ao::lmdb::Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
  auto wtxn = ao::lmdb::WriteTransaction{env};
  auto dict = ao::library::DictionaryStore{ao::lmdb::Database{wtxn, "dict"}, wtxn};

  auto rockId = dict.put(wtxn, "rock");
  auto jazzId = dict.put(wtxn, "jazz");
  wtxn.commit();

  auto rockBit = std::uint32_t{1} << (rockId.value() & 31);
  auto jazzBit = std::uint32_t{1} << (jazzId.value() & 31);

  SECTION("Tag Bloom Mask For SingleTagWithDictionary")
  {
    auto plan = QueryCompiler{&dict}.compile(parse("#rock"));
    CHECK(plan.tagBloomMask == rockBit);
  }

  SECTION("Tag Bloom Mask Ors Tags Across And")
  {
    auto plan = QueryCompiler{&dict}.compile(parse("#rock and #jazz"));
    CHECK(plan.tagBloomMask == (rockBit | jazzBit));
  }

  SECTION("Tag Bloom Mask Intersects Tags Across Or")
  {
    auto plan = QueryCompiler{&dict}.compile(parse("#rock or #jazz"));
    CHECK(plan.tagBloomMask == (rockBit & jazzBit));
  }

  SECTION("Tag Bloom Mask Clears Under Not")
  {
    auto plan = QueryCompiler{&dict}.compile(parse("not #rock"));
    CHECK(plan.tagBloomMask == 0);
  }
}

TEST_CASE("ExecutionPlan - Dictionary-Backed Field Resolution")
{
  auto temp = TempDir{};
  auto env = ao::lmdb::Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
  auto wtxn = ao::lmdb::WriteTransaction{env};
  auto dict = ao::library::DictionaryStore{ao::lmdb::Database{wtxn, "dict"}, wtxn};
  auto bachId = dict.put(wtxn, "Bach");
  wtxn.commit();

  SECTION("Dictionary-Backed Equality Resolves To NumericId")
  {
    auto expr = parse("$artist = \"Bach\"");
    auto compiler = QueryCompiler{&dict};
    auto plan = compiler.compile(expr);
    REQUIRE(plan.instructions.size() >= 2);
    CHECK(plan.instructions[1].op == OpCode::LoadConstant);
    CHECK(plan.instructions[1].constValue == static_cast<std::int64_t>(bachId.value()));
    CHECK(plan.stringConstants.empty());
  }

  SECTION("Dictionary-Backed Like Keeps StringConstant")
  {
    auto expr = parse("$artist ~ \"Bach\"");
    auto compiler = QueryCompiler{&dict};
    auto plan = compiler.compile(expr);
    REQUIRE(plan.instructions.size() >= 2);
    CHECK(plan.instructions[1].op == OpCode::LoadConstant);
    // When using LIKE, we don't resolve to ID, so it should be a string constant index
    CHECK(plan.stringConstants.size() == 1);
    CHECK(plan.stringConstants[0] == "Bach");
  }

  SECTION("No Dictionary Leaves Metadata Equality As StringConstant")
  {
    auto expr = parse("$artist = \"Bach\"");
    auto compiler = QueryCompiler{}; // No dictionary
    auto plan = compiler.compile(expr);
    CHECK(plan.stringConstants.size() == 1);
    CHECK(plan.stringConstants[0] == "Bach");
  }
}
