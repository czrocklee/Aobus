// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/lmdb/TestUtils.h"
#include <ao/library/AudioCodec.h>
#include <ao/library/DictionaryStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/Expression.h>
#include <ao/query/Parser.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace ao::query::test
{
  using namespace ao::lmdb::test;

  TEST_CASE("ExecutionPlan - Compile Simple Expression", "[query][unit][execution_plan]")
  {
    auto expr = parse("$artist = Bach");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - Compile Constant True Expression", "[query][unit][execution_plan]")
  {
    // Note: matchesAll is not automatically set - it's a hint for optimization
    // The plan should still compile a constant true expression
    auto expr = parse("true");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);

    // The plan should have at least one instruction (LoadConstant)
    CHECK_FALSE(plan.instructions.empty());
  }

  TEST_CASE("ExecutionPlan - Compile Metadata Field", "[query][unit][execution_plan]")
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

  TEST_CASE("ExecutionPlan - Compile Property Field", "[query][unit][execution_plan]")
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

  TEST_CASE("ExecutionPlan - Property Alias Maps To Bitrate Field", "[query][unit][execution_plan]")
  {
    auto expr = parse("@br >= 320k");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);

    REQUIRE_FALSE(plan.instructions.empty());
    CHECK(plan.instructions[0].op == OpCode::LoadField);
    CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(Field::Bitrate));
  }

  TEST_CASE("ExecutionPlan - Metadata Alias Maps To AlbumArtist Field", "[query][unit][execution_plan]")
  {
    auto expr = parse("$aa = Bach");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);

    REQUIRE_FALSE(plan.instructions.empty());
    CHECK(plan.instructions[0].op == OpCode::LoadField);
    CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(Field::AlbumArtistId));
  }

  TEST_CASE("ExecutionPlan - Duration Unit Constant", "[query][unit][execution_plan]")
  {
    auto expr = parse("@duration >= 3m");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);

    auto it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);

    REQUIRE(it != plan.instructions.end());
    CHECK(it->constValue == 180000);
  }

  TEST_CASE("ExecutionPlan - Bitrate Unit Constant", "[query][unit][execution_plan]")
  {
    auto expr = parse("@bitrate >= 2m");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);

    auto it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);

    REQUIRE(it != plan.instructions.end());
    CHECK(it->constValue == 2000000);
  }

  TEST_CASE("ExecutionPlan - SampleRate Unit Constant", "[query][unit][execution_plan]")
  {
    auto expr = parse("@sampleRate = 44.1k");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);

    auto it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);

    REQUIRE(it != plan.instructions.end());
    CHECK(it->constValue == 44100);
  }

  TEST_CASE("ExecutionPlan - Codec Constant", "[query][unit][execution_plan]")
  {
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(parse("@codec = AAC"));

    auto it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);

    REQUIRE(it != plan.instructions.end());
    CHECK(std::cmp_equal(it->constValue, library::audioCodecStorageValue(library::AudioCodec::Aac)));
  }

  TEST_CASE("ExecutionPlan - Unsupported Codec Constant", "[query][unit][execution_plan]")
  {
    auto compiler = QueryCompiler{};

    REQUIRE_THROWS(compiler.compile(parse("@codec = OPUS")));
  }

  TEST_CASE("ExecutionPlan - Unit Constant Rejects Unsupported Field", "[query][unit][execution_plan]")
  {
    auto expr = parse("$year >= 3m");
    auto compiler = QueryCompiler{};

    REQUIRE_THROWS(compiler.compile(expr));
  }

  TEST_CASE("ExecutionPlan - Compile Logical And", "[query][unit][execution_plan]")
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

  TEST_CASE("ExecutionPlan - Compile Logical Or", "[query][unit][execution_plan]")
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

  TEST_CASE("ExecutionPlan - Compile In List", "[query][unit][execution_plan]")
  {
    auto compiler = QueryCompiler{};

    SECTION("CompilesEachListItemAsEquality")
    {
      auto const plan = compiler.compile(parse("$year in [1990, 1991, 1992]"));

      auto const eqCount = std::ranges::count(plan.instructions, OpCode::Eq, &Instruction::op);
      auto const orCount = std::ranges::count(plan.instructions, OpCode::Or, &Instruction::op);

      CHECK(eqCount == 3);
      CHECK(orCount == 2);
    }

    SECTION("RejectsStandaloneList")
    {
      REQUIRE_THROWS(compiler.compile(parse("[1990, 1991]")));
    }

    SECTION("RejectsNonListRightOperand")
    {
      REQUIRE_THROWS(compiler.compile(parse("$artist in Bach")));
    }
  }

  TEST_CASE("ExecutionPlan - Compile Logical Not", "[query][unit][execution_plan]")
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

  TEST_CASE("ExecutionPlan - Compile Relational Operators", "[query][unit][execution_plan]")
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
    compiler = QueryCompiler{};
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

  TEST_CASE("ExecutionPlan - Compile Like", "[query][unit][execution_plan]")
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

  TEST_CASE("ExecutionPlan - Compile String Constant", "[query][unit][execution_plan]")
  {
    auto expr = parse("$title = 'Hello World'");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);

    CHECK_FALSE(plan.stringConstants.empty());
    CHECK(plan.stringConstants[0] == "Hello World");
  }

  TEST_CASE("ExecutionPlan - Field Enum Values", "[query][unit][execution_plan]")
  {
    // String fields
    CHECK(static_cast<std::uint8_t>(Field::Title) == 0);
    CHECK(static_cast<std::uint8_t>(Field::Uri) == 1);

    // Property fields
    CHECK(static_cast<std::uint8_t>(Field::Duration) == 2);
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

  TEST_CASE("ExecutionPlan - OpCode Enum Values", "[query][unit][execution_plan]")
  {
    CHECK(static_cast<std::uint8_t>(OpCode::Nop) == 0);
    CHECK(static_cast<std::uint8_t>(OpCode::LoadField) == 1);
    CHECK(static_cast<std::uint8_t>(OpCode::LoadConstant) == 2);
    CHECK(static_cast<std::uint8_t>(OpCode::Eq) == 3);
    CHECK(static_cast<std::uint8_t>(OpCode::Ne) == 4);
  }

  TEST_CASE("ExecutionPlan - AccessProfile HotOnly", "[query][unit][execution_plan]")
  {
    // Metadata variable -> HotOnly
    auto expr = parse("$artist = Bach");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);

    CHECK(plan.accessProfile == AccessProfile::HotOnly);
  }

  TEST_CASE("ExecutionPlan - AccessProfile ColdOnly", "[query][unit][execution_plan]")
  {
    // Custom variable -> ColdOnly
    auto expr = parse("%customkey = value");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);

    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - AccessProfile HotAndCold", "[query][unit][execution_plan]")
  {
    // Mix of hot and cold -> HotAndCold
    auto expr = parse("$artist = Bach && %customkey = value");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);

    CHECK(plan.accessProfile == AccessProfile::HotAndCold);
  }

  TEST_CASE("ExecutionPlan - AccessProfile Property Field", "[query][unit][execution_plan]")
  {
    // Property variable -> ColdOnly (stored in TrackColdHeader)
    auto expr = parse("@duration > 180000");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);

    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - AccessProfile Tag Field", "[query][unit][execution_plan]")
  {
    // Tag variable -> HotOnly
    auto expr = parse("#rock");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);

    CHECK(plan.accessProfile == AccessProfile::HotOnly);
  }

  TEST_CASE("ExecutionPlan - AccessProfile Cold Field", "[query][unit][execution_plan]")
  {
    // TrackNumber field is in cold storage -> ColdOnly
    auto expr = parse("$trackNumber > 5");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);

    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - AccessProfile Duration is ColdOnly", "[query][unit][execution_plan]")
  {
    auto expr = parse("@duration > 180000");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);
    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - AccessProfile Bitrate is ColdOnly", "[query][unit][execution_plan]")
  {
    auto expr = parse("@bitrate > 320");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);
    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - AccessProfile SampleRate is HotOnly", "[query][unit][execution_plan]")
  {
    auto expr = parse("@sampleRate = 44100");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);
    CHECK(plan.accessProfile == AccessProfile::HotOnly);
  }

  TEST_CASE("ExecutionPlan - AccessProfile Channels is ColdOnly", "[query][unit][execution_plan]")
  {
    auto expr = parse("@channels = 2");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);
    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - AccessProfile Mixed HotAndCold", "[query][unit][execution_plan]")
  {
    // Mix of hot ($year) and cold ($trackNumber) -> HotAndCold
    auto expr = parse("$year > 2020 && $trackNumber > 5");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);

    CHECK(plan.accessProfile == AccessProfile::HotAndCold);
  }

  TEST_CASE("ExecutionPlan - AccessProfile Custom Field", "[query][unit][execution_plan]")
  {
    // Custom variable -> ColdOnly
    auto expr = parse("%customkey = value");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);

    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - LIKE operator works for ArtistId", "[query][unit][execution_plan]")
  {
    auto temp = TempDir{};
    auto env = lmdb::Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
    auto wtxn = lmdb::WriteTransaction{env};
    auto dict = library::DictionaryStore{lmdb::Database{wtxn, "dict"}, wtxn};
    dict.put(wtxn, "Johann Sebastian Bach");

    auto expr = parse(R"($artist ~ "Bach")");
    auto compiler = QueryCompiler{&dict};

    if (auto const plan = compiler.compile(expr); plan.dictionary != nullptr)
    {
      REQUIRE(plan.dictionary == &dict);
      REQUIRE(plan.stringConstants.size() == 1);
      CHECK(plan.stringConstants.front() == "Bach");
      CHECK(plan.instructions.back().op == OpCode::Like);
      CHECK(plan.instructions.back().field == static_cast<std::uint8_t>(Field::ArtistId));
    }
  }

  TEST_CASE("ExecutionPlan - LIKE operator works for AlbumId", "[query][unit][execution_plan]")
  {
    auto expr = parse(R"($album ~ "Greatest Hits")");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);

    CHECK_FALSE(plan.instructions.empty());
  }

  TEST_CASE("ExecutionPlan - LIKE operator works for GenreId", "[query][unit][execution_plan]")
  {
    auto expr = parse(R"($genre ~ "Rock")");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);

    CHECK_FALSE(plan.instructions.empty());
  }

  TEST_CASE("ExecutionPlan - LIKE operator works for AlbumArtistId", "[query][unit][execution_plan]")
  {
    auto expr = parse(R"($albumArtist ~ "Bach")");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);

    CHECK_FALSE(plan.instructions.empty());
  }

  TEST_CASE("ExecutionPlan - LIKE operator not supported for CoverArtId", "[query][unit][execution_plan]")
  {
    auto expr = parse(R"($coverArt ~ "front")");
    auto compiler = QueryCompiler{};
    REQUIRE_THROWS(compiler.compile(expr));
  }

  TEST_CASE("ExecutionPlan - LIKE operator not supported for Tags", "[query][unit][execution_plan]")
  {
    auto expr = parse(R"(#rock ~ "progressive")");
    auto compiler = QueryCompiler{};
    REQUIRE_THROWS(compiler.compile(expr));
  }

  TEST_CASE("ExecutionPlan - LIKE operator works for Title", "[query][unit][execution_plan]")
  {
    auto expr = parse(R"($title ~ "Bach")");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - Unknown Metadata Field Throws", "[query][unit][execution_plan]")
  {
    auto expr = parse("$uri = 'x'");
    auto compiler = QueryCompiler{};
    REQUIRE_THROWS(compiler.compile(expr));
  }

  TEST_CASE("ExecutionPlan - Unknown Property Field Throws", "[query][unit][execution_plan]")
  {
    auto expr = parse("@tagCount > 0");
    auto compiler = QueryCompiler{};
    REQUIRE_THROWS(compiler.compile(expr));
  }

  TEST_CASE("ExecutionPlan - Add Operator Is Rejected", "[query][unit][execution_plan]")
  {
    auto expr = parse("$title + $artist");
    auto compiler = QueryCompiler{};
    REQUIRE_THROWS(compiler.compile(expr));
  }

  TEST_CASE("ExecutionPlan - Mixed LIKE and EQUAL in OR expression", "[query][unit][execution_plan]")
  {
    // This tests that leftField is correctly saved before compiling right operand
    // $title ~ "Bach" should NOT check if ArtistId is used with LIKE
    auto expr = parse(R"($title ~ "Bach" or $artist = "Bach")");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - Parenthesized LIKE and EQUAL in OR expression", "[query][unit][execution_plan]")
  {
    // Explicit grouping with parentheses should also work
    auto expr = parse(R"(($title ~ "Bach") or ($artist = "Bach"))");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - Multiple OR with ID field equality", "[query][unit][execution_plan]")
  {
    // Multiple ID field equalities in OR should compile without throwing
    auto expr = parse(R"($artist = "Bach" or $artist = "Mozart" or $album = "交响乐")");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - Title LIKE chained with AND", "[query][unit][execution_plan]")
  {
    // Title LIKE should work with AND
    auto expr = parse(R"($title ~ "Bach" and $year > 2000)");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - Future matching for tags not yet in dictionary", "[query][unit][execution_plan]")
  {
    auto temp = TempDir{};
    auto env = lmdb::Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
    auto wtxn = lmdb::WriteTransaction{env};
    auto dict = library::DictionaryStore{lmdb::Database{wtxn, "dict"}, wtxn};

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
        auto const& loadInstr = plan.instructions[static_cast<std::size_t>(&instr - plan.instructions.data()) - 1U];

        if (loadInstr.op == OpCode::LoadConstant)
        {
          CHECK(std::cmp_equal(loadInstr.constValue, futureTagId.raw()));
          foundTagEq = true;
        }
      }
    }

    CHECK(foundTagEq);

    // Verify Bloom Filter also contains the bit for this getOrInternd ID
    std::uint32_t const expectedBit = std::uint32_t{1} << (futureTagId.raw() & 31); // 31 is kBloomBitMask
    CHECK((plan.tagBloomMask & expectedBit) == expectedBit);
  }

  TEST_CASE("ExecutionPlan - Future matching for custom fields not yet in dictionary", "[query][unit][execution_plan]")
  {
    auto temp = TempDir{};
    auto env = lmdb::Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
    auto wtxn = lmdb::WriteTransaction{env};
    auto dict = library::DictionaryStore{lmdb::Database{wtxn, "dict"}, wtxn};

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
        CHECK(std::cmp_equal(instr.constValue, futureKeyId.raw()));
        foundLoadField = true;
      }
    }

    CHECK(foundLoadField);
  }

  TEST_CASE("ExecutionPlan - Metadata Dispatch Maps Every Supported Name", "[query][unit][execution_plan]")
  {
    struct Case final
    {
      std::string name;
      Field expected;
    };
    auto cases = {Case{.name = "year", .expected = Field::Year},
                  Case{.name = "y", .expected = Field::Year},
                  Case{.name = "trackNumber", .expected = Field::TrackNumber},
                  Case{.name = "tn", .expected = Field::TrackNumber},
                  Case{.name = "trackTotal", .expected = Field::TrackTotal},
                  Case{.name = "tt", .expected = Field::TrackTotal},
                  Case{.name = "discNumber", .expected = Field::DiscNumber},
                  Case{.name = "dn", .expected = Field::DiscNumber},
                  Case{.name = "discTotal", .expected = Field::DiscTotal},
                  Case{.name = "td", .expected = Field::DiscTotal},
                  Case{.name = "artist", .expected = Field::ArtistId},
                  Case{.name = "a", .expected = Field::ArtistId},
                  Case{.name = "album", .expected = Field::AlbumId},
                  Case{.name = "al", .expected = Field::AlbumId},
                  Case{.name = "genre", .expected = Field::GenreId},
                  Case{.name = "g", .expected = Field::GenreId},
                  Case{.name = "composer", .expected = Field::ComposerId},
                  Case{.name = "c", .expected = Field::ComposerId},
                  Case{.name = "albumArtist", .expected = Field::AlbumArtistId},
                  Case{.name = "aa", .expected = Field::AlbumArtistId},
                  Case{.name = "coverArt", .expected = Field::CoverArtId},
                  Case{.name = "ca", .expected = Field::CoverArtId},
                  Case{.name = "title", .expected = Field::Title},
                  Case{.name = "t", .expected = Field::Title},
                  Case{.name = "work", .expected = Field::WorkId},
                  Case{.name = "w", .expected = Field::WorkId}};

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

  TEST_CASE("ExecutionPlan - Property Dispatch Maps Every Supported Name", "[query][unit][execution_plan]")
  {
    struct Case final
    {
      std::string name;
      Field expected;
    };
    auto cases = {Case{.name = "duration", .expected = Field::Duration},
                  Case{.name = "l", .expected = Field::Duration},
                  Case{.name = "bitrate", .expected = Field::Bitrate},
                  Case{.name = "br", .expected = Field::Bitrate},
                  Case{.name = "sampleRate", .expected = Field::SampleRate},
                  Case{.name = "sr", .expected = Field::SampleRate},
                  Case{.name = "channels", .expected = Field::Channels},
                  Case{.name = "bitDepth", .expected = Field::BitDepth},
                  Case{.name = "bd", .expected = Field::BitDepth},
                  Case{.name = "codec", .expected = Field::Codec}};

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

  TEST_CASE("ExecutionPlan - AccessProfile Exhaustive Classification", "[query][unit][execution_plan]")
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
                     "@sampleRate",
                     "@bitDepth",
                     "@codec"};

      for (auto const* f : fields)
      {
        auto expr = parse(f);

        if (f[0] != '#' && std::string{f} != "true" && std::string{f} != "false")
        {
          expr = parse(std::string{f} + " = 0");
        }

        auto plan = compiler.compile(expr);
        CHECK(plan.accessProfile == AccessProfile::HotOnly);
      }
    }

    SECTION("ColdOnly")
    {
      auto fields = {"$trackNumber",
                     "$trackTotal",
                     "$discNumber",
                     "$discTotal",
                     "$coverArt",
                     "$work",
                     "%isrc",
                     "@duration",
                     "@bitrate",
                     "@channels"};

      for (auto const* f : fields)
      {
        auto expr = parse(std::string{f} + " >= 0");
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

  TEST_CASE("ExecutionPlan - Boolean False Compiles To ConstantZero", "[query][unit][execution_plan]")
  {
    auto expr = parse("false");
    auto compiler = QueryCompiler{};
    auto plan = compiler.compile(expr);
    REQUIRE_FALSE(plan.instructions.empty());
    CHECK(plan.instructions[0].op == OpCode::LoadConstant);
    CHECK(plan.instructions[0].constValue == 0);
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - Invalid AST Nodes Throw", "[query][unit][execution_plan]")
  {
    auto compiler = QueryCompiler{};

    SECTION("Unsupported variable type")
    {
      auto var = VariableExpression{.type = static_cast<VariableType>(99), .name = "invalid"};
      REQUIRE_THROWS(compiler.compile(var));
    }

    SECTION("Unsupported operator in BinaryExpression")
    {
      auto binaryPtr = std::make_unique<BinaryExpression>();
      binaryPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "title"};
      binaryPtr->optOperation =
        BinaryExpression::Operation{.op = Operator::Add, .operand = ConstantExpression{std::int64_t(100)}};

      REQUIRE_THROWS(compiler.compile(std::move(binaryPtr)));

      auto binaryInvalidPtr = std::make_unique<BinaryExpression>();
      binaryInvalidPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "title"};
      binaryInvalidPtr->optOperation =
        BinaryExpression::Operation{.op = static_cast<Operator>(99), .operand = ConstantExpression{std::int64_t(100)}};

      REQUIRE_THROWS(compiler.compile(std::move(binaryInvalidPtr)));
    }

    SECTION("Compiler rejects unsupported unary operators")
    {
      auto unaryPtr = std::make_unique<UnaryExpression>();
      unaryPtr->op = Operator::Add; // Unsupported unary operator
      unaryPtr->operand = VariableExpression{.type = VariableType::Tag, .name = "rock"};

      REQUIRE_THROWS(compiler.compile(std::move(unaryPtr)));
    }
  }

  TEST_CASE("ExecutionPlan - String Constant Deduplication", "[query][unit][execution_plan]")
  {
    SECTION("Reuses Identical String Constants")
    {
      auto expr = parse(R"($title = "Bach" or $title != "Bach")");
      auto compiler = QueryCompiler{};
      auto plan = compiler.compile(expr);
      CHECK(plan.stringConstants.size() == 1);
      CHECK(plan.stringConstants[0] == "Bach");
    }

    SECTION("Stores Different String Constants Separately")
    {
      auto expr = parse(R"($title = "Bach" or $title = "Mozart")");
      auto compiler = QueryCompiler{};
      auto plan = compiler.compile(expr);
      CHECK(plan.stringConstants.size() == 2);
    }
  }

  TEST_CASE("ExecutionPlan - Unit Literal Scaling", "[query][unit][execution_plan]")
  {
    auto compiler = QueryCompiler{};

    SECTION("Duration Supports MsSMMHUnits")
    {
      struct Case final
      {
        std::string unit;
        std::int64_t expected;
      };
      auto cases = {Case{.unit = "1ms", .expected = 1},
                    Case{.unit = "1s", .expected = 1000},
                    Case{.unit = "1m", .expected = 60000},
                    Case{.unit = "1h", .expected = 3600000}};

      for (auto const& c : cases)
      {
        auto expr = parse("@duration >= " + c.unit);
        auto plan = compiler.compile(expr);
        auto it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);
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

  TEST_CASE("ExecutionPlan - Unit Literal Error Paths", "[query][unit][execution_plan]")
  {
    auto compiler = QueryCompiler{};

    SECTION("Rejects UnsupportedSuffixForField")
    {
      REQUIRE_THROWS(compiler.compile(parse("@duration >= 10k")));
      REQUIRE_THROWS(compiler.compile(parse("@bitrate >= 3h")));
      REQUIRE_THROWS(compiler.compile(parse("@sampleRate >= 44h")));
      REQUIRE_THROWS(compiler.compile(parse("@channels = 2h")));
      REQUIRE_THROWS(compiler.compile(parse("@bitDepth = 16h")));
      REQUIRE_THROWS(compiler.compile(parse("$year = 2020h")));
      REQUIRE_THROWS(compiler.compile(parse("$trackNumber = 1h")));
      REQUIRE_THROWS(compiler.compile(parse("$trackTotal = 10h")));
      REQUIRE_THROWS(compiler.compile(parse("$discNumber = 1h")));
      REQUIRE_THROWS(compiler.compile(parse("$discTotal = 2h")));
      REQUIRE_THROWS(compiler.compile(parse("%custom = 1h")));
    }

    SECTION("Rejects OutOfRangeIntegerParsing")
    {
      REQUIRE_THROWS(compiler.compile(parse("@bitrate >= 9999999999999999999999k")));

      // checkedMul overflow (value * 1000 overflows)
      REQUIRE_THROWS(compiler.compile(parse("@duration >= 1844674407370955161s")));

      // checkedAdd overflow (value * 10 + fraction overflows)
      REQUIRE_THROWS(compiler.compile(parse("@duration >= 1844674407370955161.6ms")));
    }

    SECTION("Rejects NonIntegerResolution")
    {
      REQUIRE_THROWS(compiler.compile(parse("@duration >= 1.5ms")));
    }

    SECTION("Accepts Zero")
    {
      auto plan = compiler.compile(parse("@duration >= 0s"));
      CHECK(plan.instructions[1].constValue == 0);
    }

    SECTION("Rejects MissingNumericFieldContext")
    {
      // A top-level unit constant expression like "3m" should fail
      REQUIRE_THROWS(compiler.compile(parse("3m")));
    }
  }

  TEST_CASE("ExecutionPlan - Tag Bloom Mask Compilation", "[query][unit][execution_plan]")
  {
    auto temp = TempDir{};
    auto env = lmdb::Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
    auto wtxn = lmdb::WriteTransaction{env};
    auto dict = library::DictionaryStore{lmdb::Database{wtxn, "dict"}, wtxn};

    auto rockId = dict.put(wtxn, "rock");
    auto jazzId = dict.put(wtxn, "jazz");
    wtxn.commit();

    std::uint32_t const rockBit = std::uint32_t{1} << (rockId.raw() & 31);
    std::uint32_t const jazzBit = std::uint32_t{1} << (jazzId.raw() & 31);

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

  TEST_CASE("ExecutionPlan - Dictionary-Backed Field Resolution", "[query][unit][execution_plan]")
  {
    auto temp = TempDir{};
    auto env = lmdb::Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
    auto wtxn = lmdb::WriteTransaction{env};
    auto dict = library::DictionaryStore{lmdb::Database{wtxn, "dict"}, wtxn};
    auto bachId = dict.put(wtxn, "Bach");
    wtxn.commit();

    SECTION("Dictionary-Backed Equality Resolves To NumericId")
    {
      auto expr = parse("$artist = \"Bach\"");
      auto compiler = QueryCompiler{&dict};
      auto plan = compiler.compile(expr);
      REQUIRE(plan.instructions.size() >= 2);
      CHECK(plan.instructions[1].op == OpCode::LoadConstant);
      CHECK(std::cmp_equal(plan.instructions[1].constValue, bachId.raw()));
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
} // namespace ao::query::test
