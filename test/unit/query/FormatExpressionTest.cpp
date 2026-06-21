// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/lmdb/TestUtils.h"
#include <ao/AudioCodec.h>
#include <ao/Exception.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackView.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
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
#include <utility>
#include <vector>

namespace ao::query::test
{
  namespace
  {
    using namespace ao::library;
    using namespace ao::lmdb;
    using namespace ao::lmdb::test;

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
        _optEnv.emplace(_temp.path(), envOpts);
        auto wtxn = WriteTransaction{*_optEnv};
        _optDict.emplace(Database{wtxn, "dict"}, wtxn);
        _optResources.emplace(Database{wtxn, "resources"});

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

        _hotData = builder.serializeHot(wtxn, *_optDict);
        _coldData = builder.serializeCold(wtxn, *_optDict, *_optResources);
      }

      TrackView view() const { return TrackView{_hotData, _coldData}; }
      TrackView hotOnlyView() const { return TrackView{_hotData, std::span<std::byte const>{}}; }
      TrackView coldOnlyView() const { return TrackView{std::span<std::byte const>{}, _coldData}; }
      DictionaryStore& dictionary() { return *_optDict; }

    private:
      TempDir _temp;
      std::optional<Environment> _optEnv;
      std::optional<DictionaryStore> _optDict;
      std::optional<ResourceStore> _optResources;
      std::vector<std::byte> _hotData;
      std::vector<std::byte> _coldData;
    };

    std::string evaluate(std::string_view expression, FormatTrackFixture& fixture)
    {
      auto ast = parse(expression);
      auto compiler = FormatCompiler{&fixture.dictionary()};
      auto plan = compiler.compile(ast);
      auto evaluator = FormatEvaluator{};
      return evaluator.evaluate(plan, fixture.view());
    }
  } // namespace

  TEST_CASE("FormatExpression - Concatenates fields and literals", "[query][unit][format_expression]")
  {
    auto fixture = FormatTrackFixture{};

    CHECK(evaluate(R"($artist + " - " + $title)", fixture) == "Johann Sebastian Bach - Cello Suite");
    CHECK(evaluate(R"($albumArtist + "/" + $year + " - " + $album)", fixture) == "Bach/1720 - Solo Works");
    CHECK(evaluate(R"($artist " - " $title)", fixture) == "Johann Sebastian Bach - Cello Suite");
  }

  TEST_CASE("FormatExpression - Formats cold fields custom metadata and properties", "[query][unit][format_expression]")
  {
    auto fixture = FormatTrackFixture{};

    CHECK(evaluate(R"($trackNumber + "/" + $trackTotal + " " + $work)", fixture) == "3/6 BWV 1007");
    CHECK(evaluate(R"(@codec + " " + @sampleRate + "Hz " + @bitDepth + "bit")", fixture) == "FLAC 96000Hz 24bit");
    CHECK(evaluate(R"(%catalog + " " + @duration)", fixture) == "Archiv 123 143000");
  }

  TEST_CASE("FormatExpression - Unknown codec formats as empty", "[query][unit][format_expression]")
  {
    auto fixture = FormatTrackFixture{FormatTrackSpec{.codec = AudioCodec::Unknown}};

    CHECK(evaluate(R"("[" + @codec + "]")", fixture) == "[]");
  }

  TEST_CASE("FormatExpression - Unit constants stay literal text", "[query][unit][format_expression]")
  {
    auto fixture = FormatTrackFixture{};

    CHECK(evaluate(R"($title + " " + 3m)", fixture) == "Cello Suite 3m");
  }

  TEST_CASE("FormatExpression - Shares query literal keyword tokenization", "[query][unit][format_expression]")
  {
    auto fixture = FormatTrackFixture{};

    CHECK(evaluate(R"(TRUE + " " + False)", fixture) == "true false");
    CHECK(evaluate(R"('AND' + " " + "Or")", fixture) == "AND Or");
    CHECK_THROWS_AS(evaluate("AND", fixture), Exception);
  }

  TEST_CASE("FormatExpression - Missing values format as empty strings", "[query][unit][format_expression]")
  {
    auto fixture = FormatTrackFixture{FormatTrackSpec{
      .albumArtist = {},
      .work = {},
      .trackNumber = 0,
      .customPairs = {},
    }};

    CHECK(evaluate(R"($albumArtist + "-" + $work + "-" + $trackNumber + "-" + %catalog)", fixture) == "---");
  }

  TEST_CASE("FormatExpression - Reports access profile", "[query][unit][format_expression]")
  {
    auto fixture = FormatTrackFixture{};

    SECTION("HotOnly")
    {
      auto ast = parse(R"($artist + " - " + $title)");
      auto compiler = FormatCompiler{&fixture.dictionary()};
      auto plan = compiler.compile(ast);
      CHECK(plan.accessProfile == AccessProfile::HotOnly);
    }

    SECTION("ColdOnly")
    {
      auto ast = parse(R"($trackNumber + %catalog)");
      auto compiler = FormatCompiler{&fixture.dictionary()};
      auto plan = compiler.compile(ast);
      CHECK(plan.accessProfile == AccessProfile::ColdOnly);
    }

    SECTION("HotAndCold")
    {
      auto ast = parse(R"($artist + " - " + $trackNumber)");
      auto compiler = FormatCompiler{&fixture.dictionary()};
      auto plan = compiler.compile(ast);
      CHECK(plan.accessProfile == AccessProfile::HotAndCold);
    }

    SECTION("NoTrackData")
    {
      auto ast = parse(R"("literal")");
      auto compiler = FormatCompiler{};
      auto plan = compiler.compile(ast);
      CHECK(plan.accessProfile == AccessProfile::NoTrackData);
    }
  }

  TEST_CASE("FormatExpression - Literal-only plans do not require track data", "[query][unit][format_expression]")
  {
    auto ast = parse(R"("literal")");
    auto compiler = FormatCompiler{};
    auto plan = compiler.compile(ast);
    auto evaluator = FormatEvaluator{};
    auto emptyTrack = TrackView{std::span<std::byte const>{}, std::span<std::byte const>{}};

    CHECK(evaluator.evaluate(plan, emptyTrack) == "literal");
  }

  TEST_CASE("FormatExpression - Returns empty when required track data is missing", "[query][unit][format_expression]")
  {
    auto fixture = FormatTrackFixture{};
    auto evaluator = FormatEvaluator{};

    SECTION("Hot plan with cold-only track")
    {
      auto ast = parse("$artist");
      auto compiler = FormatCompiler{&fixture.dictionary()};
      auto plan = compiler.compile(ast);
      CHECK(evaluator.evaluate(plan, fixture.coldOnlyView()).empty());
    }

    SECTION("Cold plan with hot-only track")
    {
      auto ast = parse("$trackNumber");
      auto compiler = FormatCompiler{&fixture.dictionary()};
      auto plan = compiler.compile(ast);
      CHECK(evaluator.evaluate(plan, fixture.hotOnlyView()).empty());
    }
  }

  TEST_CASE("FormatExpression - Rejects query-only expressions", "[query][unit][format_expression]")
  {
    auto fixture = FormatTrackFixture{};
    auto compiler = FormatCompiler{&fixture.dictionary()};

    CHECK_THROWS_AS(compiler.compile(parse("$artist = Bach")), Exception);
    CHECK_THROWS_AS(compiler.compile(parse("$year?")), Exception);
    CHECK_THROWS_AS(compiler.compile(parse("not $title")), Exception);
    CHECK_THROWS_AS(compiler.compile(parse("$year in 1720..1730")), Exception);
  }

  TEST_CASE("FormatExpression - Rejects non-scalar fields", "[query][unit][format_expression]")
  {
    auto fixture = FormatTrackFixture{};
    auto compiler = FormatCompiler{&fixture.dictionary()};

    CHECK_THROWS_AS(compiler.compile(parse("#favorite")), Exception);
    CHECK_THROWS_AS(compiler.compile(parse("$coverArt")), Exception);
  }

  TEST_CASE("FormatExpression - Requires dictionary for dictionary backed fields", "[query][unit][format_expression]")
  {
    auto compiler = FormatCompiler{};

    CHECK_THROWS_AS(compiler.compile(parse("$artist")), Exception);
    CHECK_THROWS_AS(compiler.compile(parse("%catalog")), Exception);
    CHECK_NOTHROW(compiler.compile(parse(R"($title + " " + $year)")));
  }
} // namespace ao::query::test
