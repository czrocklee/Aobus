// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/query/ExecutionPlanTestSupport.h"
#include <ao/query/Field.h>
#include <ao/query/detail/Bytecode.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

namespace ao::query::test
{
  TEST_CASE("ExecutionPlan - maps property aliases to bitrate fields", "[query][unit][execution-plan][catalog]")
  {
    auto expr = parseOk("@br >= 320k");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    REQUIRE_FALSE(plan.instructions.empty());
    CHECK(plan.instructions[0].op == OpCode::LoadField);
    CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(Field::Bitrate));
  }

  TEST_CASE("ExecutionPlan - maps metadata aliases to album artist fields", "[query][unit][execution-plan][catalog]")
  {
    auto expr = parseOk("$aa = Bach");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    REQUIRE_FALSE(plan.instructions.empty());
    CHECK(plan.instructions[0].op == OpCode::LoadField);
    CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(Field::AlbumArtistId));
  }

  TEST_CASE("ExecutionPlan - rejects unknown metadata fields", "[query][unit][execution-plan][catalog]")
  {
    auto expr = parseOk("$gerne = 'x'");
    auto compiler = QueryCompiler{};
    auto const error = compileError(compiler, expr);
    CHECK(error.message.contains("did you mean '$genre'?"));
    CHECK(error.message.contains("available metadata fields:"));
    CHECK(error.message.contains("$movement ($m)"));
  }

  TEST_CASE("ExecutionPlan - rejects unknown property fields", "[query][unit][execution-plan][catalog]")
  {
    auto expr = parseOk("@samplerate > 0");
    auto compiler = QueryCompiler{};
    auto const error = compileError(compiler, expr);
    CHECK(error.message.contains("did you mean '@sampleRate'?"));
    CHECK(error.message.contains("available property fields:"));
  }

  TEST_CASE("ExecutionPlan - rejects track ids as query fields", "[query][unit][execution-plan][catalog]")
  {
    auto expr = parseOk("$id = 1");
    auto compiler = QueryCompiler{};
    auto const error = compileError(compiler, expr);
    CHECK(error.message.contains("unknown metadata field '$id'"));
  }

  TEST_CASE("ExecutionPlan - maps every supported metadata catalog name", "[query][unit][execution-plan][catalog]")
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
                  Case{.name = "movementNumber", .expected = Field::MovementNumber},
                  Case{.name = "mn", .expected = Field::MovementNumber},
                  Case{.name = "movementTotal", .expected = Field::MovementTotal},
                  Case{.name = "mt", .expected = Field::MovementTotal},
                  Case{.name = "artist", .expected = Field::ArtistId},
                  Case{.name = "a", .expected = Field::ArtistId},
                  Case{.name = "album", .expected = Field::AlbumId},
                  Case{.name = "al", .expected = Field::AlbumId},
                  Case{.name = "genre", .expected = Field::GenreId},
                  Case{.name = "g", .expected = Field::GenreId},
                  Case{.name = "composer", .expected = Field::ComposerId},
                  Case{.name = "c", .expected = Field::ComposerId},
                  Case{.name = "conductor", .expected = Field::ConductorId},
                  Case{.name = "ensemble", .expected = Field::EnsembleId},
                  Case{.name = "albumArtist", .expected = Field::AlbumArtistId},
                  Case{.name = "aa", .expected = Field::AlbumArtistId},
                  Case{.name = "coverArt", .expected = Field::CoverArtId},
                  Case{.name = "ca", .expected = Field::CoverArtId},
                  Case{.name = "title", .expected = Field::Title},
                  Case{.name = "t", .expected = Field::Title},
                  Case{.name = "work", .expected = Field::WorkId},
                  Case{.name = "w", .expected = Field::WorkId},
                  Case{.name = "movement", .expected = Field::MovementId},
                  Case{.name = "m", .expected = Field::MovementId},
                  Case{.name = "soloist", .expected = Field::SoloistId}};

    for (auto const& c : cases)
    {
      DYNAMIC_SECTION("Field: " << c.name)
      {
        auto expr = parseOk("$" + c.name + " = 'x'");
        auto compiler = QueryCompiler{};
        auto plan = compileOk(compiler, expr);
        REQUIRE_FALSE(plan.instructions.empty());
        CHECK(plan.instructions[0].op == OpCode::LoadField);
        CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(c.expected));
      }
    }
  }

  TEST_CASE("ExecutionPlan - maps every supported property catalog name", "[query][unit][execution-plan][catalog]")
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
        auto expr = parseOk("@" + c.name + " >= 0");
        auto compiler = QueryCompiler{};
        auto plan = compileOk(compiler, expr);
        REQUIRE_FALSE(plan.instructions.empty());
        CHECK(plan.instructions[0].op == OpCode::LoadField);
        CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(c.expected));
      }
    }
  }
} // namespace ao::query::test
