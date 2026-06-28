// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/lmdb/TestUtils.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackView.h>
#include <ao/lmdb/Environment.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/FormatExpression.h>
#include <ao/query/Parser.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
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
    using namespace ao::library;
    using namespace ao::lmdb;
    using namespace ao::lmdb::test;

    Expression parseOk(std::string_view text)
    {
      auto result = ::ao::query::parse(text);
      REQUIRE(result.has_value());
      return std::move(*result);
    }

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

    struct FormatTrackSpec final
    {
      std::string title = "Cello Suite";
      std::string artist = "Johann Sebastian Bach";
      std::string album = "Solo Works";
      std::string albumArtist = "Bach";
      std::string genre = "Classical";
      std::string composer = "Bach";
      std::string work = "BWV 1007";
      std::uint16_t year = 1720;
      std::uint16_t trackNumber = 3;
      std::uint16_t trackTotal = 6;
      std::uint16_t discNumber = 1;
      std::uint16_t discTotal = 2;
      std::chrono::milliseconds duration = std::chrono::seconds{143};
      std::uint32_t bitrate = 912000;
      std::uint32_t sampleRate = 96000;
      std::uint8_t channels = 2;
      std::uint8_t bitDepth = 24;
      AudioCodec codec = AudioCodec::Flac;
      std::vector<std::pair<std::string, std::string>> customPairs = {{"catalog", "Archiv 123"}};
    };

    class FormatTrackFixture final
    {
    public:
      explicit FormatTrackFixture(FormatTrackSpec const& spec = {})
      {
        auto envOpts = Environment::Options{.flags = MDB_CREATE, .maxDatabases = 20};
        _optEnv.emplace(openEnvironment(_temp.path(), envOpts));
        auto wtxn = beginWriteTransaction(*_optEnv);
        _optDict.emplace(openDatabase(wtxn, "dict"), wtxn);
        _optResources.emplace(openDatabase(wtxn, "resources"));

        auto builder = TrackBuilder::createNew();
        builder.metadata()
          .title(spec.title)
          .artist(spec.artist)
          .album(spec.album)
          .albumArtist(spec.albumArtist)
          .genre(spec.genre)
          .composer(spec.composer)
          .work(spec.work)
          .year(spec.year)
          .trackNumber(spec.trackNumber)
          .trackTotal(spec.trackTotal)
          .discNumber(spec.discNumber)
          .discTotal(spec.discTotal);
        builder.property()
          .duration(spec.duration)
          .bitrate(Bitrate{spec.bitrate})
          .sampleRate(SampleRate{spec.sampleRate})
          .channels(Channels{spec.channels})
          .bitDepth(BitDepth{spec.bitDepth})
          .codec(spec.codec);

        for (auto const& [key, value] : spec.customPairs)
        {
          builder.customMetadata().add(key, value);
        }

        auto hotDataResult = builder.serializeHot(wtxn, *_optDict);
        REQUIRE(hotDataResult);
        auto coldDataResult = builder.serializeCold(wtxn, *_optDict, *_optResources);
        REQUIRE(coldDataResult);
        _hotData = *hotDataResult;
        _coldData = *coldDataResult;
      }

      TrackView view() const { return TrackView{_hotData, _coldData}; }
      TrackView hotOnlyView() const { return TrackView{_hotData, std::span<std::byte const>{}}; }
      TrackView coldOnlyView() const { return TrackView{std::span<std::byte const>{}, _coldData}; }
      DictionaryStore& dictionary() { return *_optDict; }

    private:
      ao::test::TempDir _temp;
      std::optional<Environment> _optEnv;
      std::optional<DictionaryStore> _optDict;
      std::optional<ResourceStore> _optResources;
      std::vector<std::byte> _hotData;
      std::vector<std::byte> _coldData;
    };

    std::string evaluate(std::string_view expression, FormatTrackFixture& fixture)
    {
      auto ast = parseOk(expression);
      auto compiler = FormatCompiler{&fixture.dictionary()};
      auto plan = compileOk(compiler, ast);
      auto evaluator = FormatEvaluator{};
      return evaluator.evaluate(plan, fixture.view());
    }
  } // namespace

  TEST_CASE("FormatExpression - concatenates fields and literals", "[query][unit][format_expression]")
  {
    auto fixture = FormatTrackFixture{};

    CHECK(evaluate(R"($artist + " - " + $title)", fixture) == "Johann Sebastian Bach - Cello Suite");
    CHECK(evaluate(R"($albumArtist + "/" + $year + " - " + $album)", fixture) == "Bach/1720 - Solo Works");
    CHECK(evaluate(R"($artist " - " $title)", fixture) == "Johann Sebastian Bach - Cello Suite");
  }

  TEST_CASE("FormatExpression - formats cold fields custom metadata and properties", "[query][unit][format_expression]")
  {
    auto fixture = FormatTrackFixture{};

    CHECK(evaluate(R"($trackNumber + "/" + $trackTotal + " " + $work)", fixture) == "3/6 BWV 1007");
    CHECK(evaluate(R"(@codec + " " + @sampleRate + "Hz " + @bitDepth + "bit")", fixture) == "FLAC 96000Hz 24bit");
    CHECK(evaluate(R"(%catalog + " " + @duration)", fixture) == "Archiv 123 143000");
  }

  TEST_CASE("FormatExpression - formats unknown codecs as empty", "[query][unit][format_expression]")
  {
    auto fixture = FormatTrackFixture{FormatTrackSpec{.codec = AudioCodec::Unknown}};

    CHECK(evaluate(R"("[" + @codec + "]")", fixture) == "[]");
  }

  TEST_CASE("FormatExpression - keeps unit constants as literal text", "[query][unit][format_expression]")
  {
    auto fixture = FormatTrackFixture{};

    CHECK(evaluate(R"($title + " " + 3m)", fixture) == "Cello Suite 3m");
  }

  TEST_CASE("FormatExpression - shares query literal keyword tokenization", "[query][unit][format_expression]")
  {
    auto fixture = FormatTrackFixture{};

    CHECK(evaluate(R"(TRUE + " " + False)", fixture) == "true false");
    CHECK(evaluate(R"('AND' + " " + "Or")", fixture) == "AND Or");
    CHECK_FALSE(::ao::query::parse("AND").has_value());
  }

  TEST_CASE("FormatExpression - formats missing values as empty strings", "[query][unit][format_expression]")
  {
    auto fixture = FormatTrackFixture{FormatTrackSpec{
      .albumArtist = {},
      .work = {},
      .trackNumber = 0,
      .customPairs = {},
    }};

    CHECK(evaluate(R"($albumArtist + "-" + $work + "-" + $trackNumber + "-" + %catalog)", fixture) == "---");
  }

  TEST_CASE("FormatExpression - reports access profile", "[query][unit][format_expression]")
  {
    auto fixture = FormatTrackFixture{};

    SECTION("HotOnly")
    {
      auto ast = parseOk(R"($artist + " - " + $title)");
      auto compiler = FormatCompiler{&fixture.dictionary()};
      auto plan = compileOk(compiler, ast);
      CHECK(plan.accessProfile == AccessProfile::HotOnly);
    }

    SECTION("ColdOnly")
    {
      auto ast = parseOk(R"($trackNumber + %catalog)");
      auto compiler = FormatCompiler{&fixture.dictionary()};
      auto plan = compileOk(compiler, ast);
      CHECK(plan.accessProfile == AccessProfile::ColdOnly);
    }

    SECTION("HotAndCold")
    {
      auto ast = parseOk(R"($artist + " - " + $trackNumber)");
      auto compiler = FormatCompiler{&fixture.dictionary()};
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

  TEST_CASE("FormatExpression - evaluates literal-only plans without track data", "[query][unit][format_expression]")
  {
    auto ast = parseOk(R"("literal")");
    auto compiler = FormatCompiler{};
    auto plan = compileOk(compiler, ast);
    auto evaluator = FormatEvaluator{};
    auto emptyTrack = TrackView{std::span<std::byte const>{}, std::span<std::byte const>{}};

    CHECK(evaluator.evaluate(plan, emptyTrack) == "literal");
  }

  TEST_CASE("FormatExpression - returns empty when required track data is missing", "[query][unit][format_expression]")
  {
    auto fixture = FormatTrackFixture{};
    auto evaluator = FormatEvaluator{};

    SECTION("Hot plan with cold-only track")
    {
      auto ast = parseOk("$artist");
      auto compiler = FormatCompiler{&fixture.dictionary()};
      auto plan = compileOk(compiler, ast);
      CHECK(evaluator.evaluate(plan, fixture.coldOnlyView()).empty());
    }

    SECTION("Cold plan with hot-only track")
    {
      auto ast = parseOk("$trackNumber");
      auto compiler = FormatCompiler{&fixture.dictionary()};
      auto plan = compileOk(compiler, ast);
      CHECK(evaluator.evaluate(plan, fixture.hotOnlyView()).empty());
    }
  }

  TEST_CASE("FormatExpression - rejects query-only expressions", "[query][unit][format_expression]")
  {
    auto fixture = FormatTrackFixture{};
    auto compiler = FormatCompiler{&fixture.dictionary()};

    std::ignore = compileError(compiler, parseOk("$artist = Bach"));
    std::ignore = compileError(compiler, parseOk("$year?"));
    std::ignore = compileError(compiler, parseOk("not $title"));
    std::ignore = compileError(compiler, parseOk("$year in 1720..1730"));
  }

  TEST_CASE("FormatExpression - rejects non-scalar fields", "[query][unit][format_expression]")
  {
    auto fixture = FormatTrackFixture{};
    auto compiler = FormatCompiler{&fixture.dictionary()};

    std::ignore = compileError(compiler, parseOk("#favorite"));
    std::ignore = compileError(compiler, parseOk("$coverArt"));
  }

  TEST_CASE("FormatExpression - requires dictionary for dictionary-backed fields", "[query][unit][format_expression]")
  {
    auto compiler = FormatCompiler{};

    std::ignore = compileError(compiler, parseOk("$artist"));
    std::ignore = compileError(compiler, parseOk("%catalog"));
    std::ignore = compileOk(compiler, parseOk(R"($title + " " + $year)"));
  }

  TEST_CASE("compileFormat returns Result without throwing", "[query][unit][format_expression]")
  {
    auto fixture = FormatTrackFixture{};

    SECTION("Valid format expression yields a plan")
    {
      auto const plan = compileFormat(parseOk(R"($title + " " + $year)"), &fixture.dictionary());
      CHECK(plan.has_value());
    }

    SECTION("Non-formattable expression yields an Error")
    {
      auto const plan = compileFormat(parseOk("#favorite"), &fixture.dictionary());
      REQUIRE_FALSE(plan.has_value());
      CHECK(plan.error().code == Error::Code::FormatRejected);
      CHECK_FALSE(plan.error().message.empty());
    }
  }
} // namespace ao::query::test
