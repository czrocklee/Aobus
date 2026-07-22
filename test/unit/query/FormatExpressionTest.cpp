// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/query/PlanEvaluatorTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/library/TrackView.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/FormatExpression.h>
#include <ao/query/Parser.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::query::test
{
  namespace
  {
    FormatPlan compileOk(FormatCompiler& compiler, Expression const& expr)
    {
      auto result = compiler.compile(expr);
      REQUIRE(result.has_value());
      return std::move(*result);
    }

    Error compileError(FormatCompiler& compiler, Expression const& expr)
    {
      auto result = compiler.compile(expr);
      REQUIRE_FALSE(result.has_value());
      CHECK(result.error().code == Error::Code::FormatRejected);
      return result.error();
    }

    TrackSpec formatTrackSpec()
    {
      return TrackSpec{.title = "Cello Suite",
                       .artist = "Johann Sebastian Bach",
                       .album = "Solo Works",
                       .albumArtist = "Bach",
                       .composer = "Bach",
                       .conductor = "Carlos Kleiber",
                       .ensemble = "Vienna Philharmonic",
                       .work = "BWV 1007",
                       .movement = "Prelude",
                       .soloist = "Yo-Yo Ma",
                       .genre = "Classical",
                       .year = 1720,
                       .trackNumber = 3,
                       .trackTotal = 6,
                       .discNumber = 1,
                       .discTotal = 2,
                       .movementNumber = 1,
                       .movementTotal = 6,
                       .duration = std::chrono::seconds{143},
                       .bitrate = 912000,
                       .sampleRate = 96000,
                       .channels = 2,
                       .bitDepth = 24,
                       .codec = AudioCodec::Flac,
                       .customPairs = {{"catalog", "Archiv 123"}}};
    }

    std::string evaluate(std::string_view expression, TrackFixture& fixture)
    {
      auto ast = parseOk(expression);
      auto compiler = FormatCompiler{};
      auto plan = compileOk(compiler, ast);
      auto cache = library::DictionaryReadCache{fixture.dictionary()};
      auto context = library::DictionaryReadContext{cache};
      auto const binding = FormatBinding{plan, context};
      auto evaluator = FormatEvaluator{};
      return evaluator.evaluate(binding, fixture.view());
    }
  } // namespace

  TEST_CASE("FormatExpression - concatenates fields and literals", "[query][unit][format-expression]")
  {
    auto fixture = TrackFixture{formatTrackSpec()};

    CHECK(evaluate(R"($artist + " - " + $title)", fixture) == "Johann Sebastian Bach - Cello Suite");
    CHECK(evaluate(R"($albumArtist + "/" + $year + " - " + $album)", fixture) == "Bach/1720 - Solo Works");
    CHECK(evaluate(R"($artist " - " $title)", fixture) == "Johann Sebastian Bach - Cello Suite");
  }

  TEST_CASE("FormatExpression - formats cold fields custom metadata and properties", "[query][unit][format-expression]")
  {
    auto fixture = TrackFixture{formatTrackSpec()};

    CHECK(evaluate(R"($trackNumber + "/" + $trackTotal + " " + $work)", fixture) == "3/6 BWV 1007");
    CHECK(evaluate(R"($movementNumber + "/" + $movementTotal + " " + $movement)", fixture) == "1/6 Prelude");
    CHECK(evaluate(R"($conductor + " / " + $ensemble + " / " + $soloist)", fixture) ==
          "Carlos Kleiber / Vienna Philharmonic / Yo-Yo Ma");
    CHECK(evaluate(R"(@codec + " " + @sampleRate + "Hz " + @bitDepth + "bit")", fixture) == "FLAC 96000Hz 24bit");
    CHECK(evaluate(R"(%catalog + " " + @duration)", fixture) == "Archiv 123 143000");
  }

  TEST_CASE("FormatExpression - rejects track ids as format fields", "[query][unit][format-expression]")
  {
    auto fixture = TrackFixture{formatTrackSpec()};
    auto ast = parseOk(R"($id + ": " + $title)");
    auto compiler = FormatCompiler{};
    auto const error = compileError(compiler, ast);
    CHECK(error.message.contains("unknown metadata field '$id'"));
  }

  TEST_CASE("FormatExpression - formats unknown codecs as empty", "[query][unit][format-expression]")
  {
    auto spec = formatTrackSpec();
    spec.codec = AudioCodec::Unknown;
    auto fixture = TrackFixture{spec};

    CHECK(evaluate(R"("[" + @codec + "]")", fixture) == "[]");
  }

  TEST_CASE("FormatExpression - keeps unit constants as literal text", "[query][unit][format-expression]")
  {
    auto fixture = TrackFixture{formatTrackSpec()};

    CHECK(evaluate(R"($title + " " + 3m)", fixture) == "Cello Suite 3m");
  }

  TEST_CASE("FormatExpression - shares query literal keyword tokenization", "[query][unit][format-expression]")
  {
    auto fixture = TrackFixture{formatTrackSpec()};

    CHECK(evaluate(R"(TRUE + " " + False)", fixture) == "true false");
    CHECK(evaluate(R"('AND' + " " + "Or")", fixture) == "AND Or");
    CHECK_FALSE(::ao::query::parse("AND").has_value());
  }

  TEST_CASE("FormatExpression - formats missing values as empty strings", "[query][unit][format-expression]")
  {
    auto spec = formatTrackSpec();
    spec.albumArtist = {};
    spec.work = {};
    spec.movement = {};
    spec.trackNumber = 0;
    spec.movementNumber = 0;
    spec.customPairs = {};
    auto fixture = TrackFixture{spec};

    CHECK(evaluate(R"($albumArtist + "-" + $work + "-" + $movement + "-" + )"
                   R"($trackNumber + "-" + $movementNumber + "-" + %catalog)",
                   fixture) == "-----");
  }

  TEST_CASE("FormatExpression - reports access profile", "[query][unit][format-expression]")
  {
    auto fixture = TrackFixture{formatTrackSpec()};

    SECTION("HotOnly")
    {
      auto ast = parseOk(R"($artist + " - " + $title)");
      auto compiler = FormatCompiler{};
      auto plan = compileOk(compiler, ast);
      CHECK(plan.accessProfile == AccessProfile::HotOnly);
    }

    SECTION("ColdOnly")
    {
      auto ast = parseOk(R"($trackNumber + %catalog)");
      auto compiler = FormatCompiler{};
      auto plan = compileOk(compiler, ast);
      CHECK(plan.accessProfile == AccessProfile::ColdOnly);
    }

    SECTION("HotAndCold")
    {
      auto ast = parseOk(R"($artist + " - " + $trackNumber)");
      auto compiler = FormatCompiler{};
      auto plan = compileOk(compiler, ast);
      CHECK(plan.accessProfile == AccessProfile::HotAndCold);
    }

    SECTION("NoTrackData")
    {
      auto ast = parseOk(R"("literal")");
      auto compiler = FormatCompiler{};
      auto plan = compileOk(compiler, ast);
      CHECK(plan.accessProfile == AccessProfile::NoTrackData);
    }
  }

  TEST_CASE("FormatExpression - evaluates literal-only plans without track data", "[query][unit][format-expression]")
  {
    auto ast = parseOk(R"("literal")");
    auto compiler = FormatCompiler{};
    auto plan = compileOk(compiler, ast);
    auto evaluator = FormatEvaluator{};
    auto emptyTrack = TrackView{std::span<std::byte const>{}, std::span<std::byte const>{}};

    CHECK(evaluator.evaluate(plan, emptyTrack) == "literal");
  }

  TEST_CASE("FormatEvaluator - clears and reuses caller-owned output", "[query][unit][format-expression]")
  {
    auto fixture = TrackFixture{formatTrackSpec()};
    auto ast = parseOk(R"($artist + "|" + $title + "|" + %catalog + "|" + $year + "|" + @codec)");
    auto compiler = FormatCompiler{};
    auto plan = compileOk(compiler, ast);
    auto evaluator = FormatEvaluator{};
    auto output = std::string{"stale data"};
    auto cache = library::DictionaryReadCache{fixture.dictionary()};
    auto context = library::DictionaryReadContext{cache};
    auto const binding = FormatBinding{plan, context};

    evaluator.evaluate(binding, fixture.view(), output);

    CHECK(output == "Johann Sebastian Bach|Cello Suite|Archiv 123|1720|FLAC");
    CHECK(output == evaluator.evaluate(binding, fixture.view()));

    output += " appended garbage";
    evaluator.evaluate(binding, fixture.view(), output);
    CHECK(output == "Johann Sebastian Bach|Cello Suite|Archiv 123|1720|FLAC");
  }

  TEST_CASE("FormatExpression - rejects query-only expressions", "[query][unit][format-expression]")
  {
    auto fixture = TrackFixture{formatTrackSpec()};
    auto compiler = FormatCompiler{};

    std::ignore = compileError(compiler, parseOk("$artist = Bach"));
    std::ignore = compileError(compiler, parseOk("$year?"));
    std::ignore = compileError(compiler, parseOk("not $title"));
    std::ignore = compileError(compiler, parseOk("$year in 1720..1730"));
  }

  TEST_CASE("FormatExpression - rejects non-scalar fields", "[query][unit][format-expression]")
  {
    auto fixture = TrackFixture{formatTrackSpec()};
    auto compiler = FormatCompiler{};

    std::ignore = compileError(compiler, parseOk("#favorite"));
    std::ignore = compileError(compiler, parseOk("$coverArt"));
  }

  TEST_CASE("FormatExpression - marks dictionary-backed plans for binding", "[query][unit][format-expression]")
  {
    auto compiler = FormatCompiler{};

    auto artist = compileOk(compiler, parseOk("$artist"));
    CHECK(artist.requiresDictionary);

    auto custom = compileOk(compiler, parseOk("%catalog"));
    CHECK(custom.requiresDictionary);
    CHECK(custom.dictionarySymbols == std::vector<std::string>{"catalog"});

    auto plain = compileOk(compiler, parseOk(R"($title + " " + $year)"));
    CHECK_FALSE(plain.requiresDictionary);
  }

  TEST_CASE("compileFormat returns Result without throwing", "[query][unit][format-expression]")
  {
    SECTION("Valid format expression yields a plan")
    {
      auto const plan = compileFormat(parseOk(R"($title + " " + $year)"));
      CHECK(plan.has_value());
    }

    SECTION("Non-formattable expression yields an Error")
    {
      auto const plan = compileFormat(parseOk("#favorite"));
      REQUIRE_FALSE(plan.has_value());
      CHECK(plan.error().code == Error::Code::FormatRejected);
      CHECK_FALSE(plan.error().message.empty());
    }
  }
} // namespace ao::query::test
