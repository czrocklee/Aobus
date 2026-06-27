// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/lmdb/TestUtils.h"
#include "test/unit/query/PlanEvaluatorTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/Type.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/TrackView.h>
#include <ao/query/Field.h>
#include <ao/query/PlanEvaluator.h>
#include <ao/query/QueryCompiler.h>
#include <ao/query/detail/Bytecode.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace ao::query::test
{
  TEST_CASE("PlanEvaluator matches work metadata equality and LIKE expressions", "[query][unit][plan_evaluator]")
  {
    auto trackWithWork = TestTrack{
      "Title", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {}, "", "Symphony No. 5"};
    auto trackWithoutWork =
      TestTrack{"Title", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {}, "", ""};
    auto evaluator = PlanEvaluator{};

    SECTION("$work Equality")
    {
      auto expr = parseOk("$work = 'Symphony No. 5'");
      auto compiler = QueryCompiler{&trackWithWork.dictionary()};
      auto plan = compileOk(compiler, expr);
      CHECK(evaluator.evaluateFull(plan, trackWithWork.view()) == true);
      CHECK(evaluator.evaluateFull(plan, trackWithoutWork.view()) == false);
    }

    SECTION("$w Equality (shorthand)")
    {
      auto expr = parseOk("$w = 'Symphony No. 5'");
      auto compiler = QueryCompiler{&trackWithWork.dictionary()};
      auto plan = compileOk(compiler, expr);
      CHECK(evaluator.evaluateFull(plan, trackWithWork.view()) == true);
    }

    SECTION("$work LIKE")
    {
      auto expr = parseOk("$work ~ Symphony");
      auto compiler = QueryCompiler{&trackWithWork.dictionary()};
      auto plan = compileOk(compiler, expr);
      CHECK(evaluator.evaluateFull(plan, trackWithWork.view()) == true);
      CHECK(evaluator.evaluateFull(plan, trackWithoutWork.view()) == false);
    }
  }

  TEST_CASE("PlanEvaluator matches composer metadata equality and LIKE expressions", "[query][unit][plan_evaluator]")
  {
    auto trackWithComposer = TestTrack{
      "Title", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {}, "Beethoven", ""};
    auto evaluator = PlanEvaluator{};

    SECTION("$composer Equality")
    {
      auto expr = parseOk("$composer = Beethoven");
      auto compiler = QueryCompiler{&trackWithComposer.dictionary()};
      auto plan = compileOk(compiler, expr);
      CHECK(evaluator.evaluateFull(plan, trackWithComposer.view()) == true);
    }

    SECTION("$composer LIKE")
    {
      auto expr = parseOk("$composer ~ Beet");
      auto compiler = QueryCompiler{&trackWithComposer.dictionary()};
      auto plan = compileOk(compiler, expr);
      CHECK(evaluator.evaluateFull(plan, trackWithComposer.view()) == true);
    }
  }

  TEST_CASE("PlanEvaluator resolves dictionary artist ids for LIKE expressions", "[query][unit][plan_evaluator]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto dict = DictionaryStore{openDatabase(wtxn, "dict"), wtxn};
    auto bachId = ao::test::requireValue(dict.put(wtxn, "Johann Sebastian Bach"));
    auto mozartId = ao::test::requireValue(dict.put(wtxn, "Wolfgang Amadeus Mozart"));

    auto expr = parseOk(R"($artist ~ "Bach")");
    auto compiler = QueryCompiler{&dict};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto matchingHotData = makeHotOnlyTrack(bachId);
    auto matchingTrack = library::TrackView{matchingHotData, std::span<std::byte const>{}};
    CHECK(evaluator.evaluateFull(plan, matchingTrack) == true);

    auto nonMatchingHotData = makeHotOnlyTrack(mozartId);
    auto nonMatchingTrack = library::TrackView{nonMatchingHotData, std::span<std::byte const>{}};
    CHECK(evaluator.evaluateFull(plan, nonMatchingTrack) == false);

    auto missingArtistHotData = makeHotOnlyTrack();
    auto missingArtistTrack = library::TrackView{missingArtistHotData, std::span<std::byte const>{}};
    CHECK(evaluator.evaluateFull(plan, missingArtistTrack) == false);
  }

  TEST_CASE("PlanEvaluator matches dictionary-backed metadata and property fields", "[query][unit][plan_evaluator]")
  {
    auto spec = TrackSpec{};
    spec.codec = AudioCodec::Flac;
    spec.trackNumber = 3;
    spec.trackTotal = 12;
    spec.discNumber = 1;
    spec.discTotal = 2;
    spec.coverArtId = ResourceId{99};
    spec.album = "Test Album";
    spec.genre = "Test Genre";
    spec.albumArtist = "Test Album Artist";
    auto track = TestTrack{spec};

    auto& dict = track.dictionary();
    auto compiler = QueryCompiler{&dict};
    auto evaluator = PlanEvaluator{};

    SECTION("Album")
    {
      auto plan = compileOk(compiler, parseOk("$album = 'Test Album'"));
      CHECK(evaluator.evaluateFull(plan, track.view()) == true);

      auto planLike = compileOk(compiler, parseOk("$album ~ 'Test'"));
      CHECK(evaluator.evaluateFull(planLike, track.view()) == true);
    }

    SECTION("Genre")
    {
      auto plan = compileOk(compiler, parseOk("$genre = 'Test Genre'"));
      CHECK(evaluator.evaluateFull(plan, track.view()) == true);

      auto planLike = compileOk(compiler, parseOk("$genre ~ 'Genre'"));
      CHECK(evaluator.evaluateFull(planLike, track.view()) == true);
    }

    SECTION("AlbumArtist")
    {
      auto plan = compileOk(compiler, parseOk("$albumArtist = 'Test Album Artist'"));
      CHECK(evaluator.evaluateFull(plan, track.view()) == true);

      auto planLike = compileOk(compiler, parseOk("$albumArtist ~ 'Album Artist'"));
      CHECK(evaluator.evaluateFull(planLike, track.view()) == true);
    }

    SECTION("Uri")
    {
      auto plan = ExecutionPlan{};
      plan.stringConstants.emplace_back("/path/to/track.flac");
      plan.instructions.push_back(
        {.op = OpCode::LoadField, .field = static_cast<std::uint8_t>(Field::Uri), .operand = 0});
      plan.instructions.push_back({.op = OpCode::LoadConstant, .operand = 1, .constValue = 0});
      plan.instructions.push_back({.op = OpCode::Eq, .field = static_cast<std::uint8_t>(Field::Uri), .operand = 1});
      CHECK(evaluator.evaluateFull(plan, track.view()) == true);
    }

    SECTION("Channels")
    {
      auto plan = compileOk(compiler, parseOk("@channels = 2"));
      CHECK(evaluator.evaluateFull(plan, track.view()) == true);
    }

    SECTION("BitDepth")
    {
      auto plan = compileOk(compiler, parseOk("@bitDepth = 16"));
      CHECK(evaluator.evaluateFull(plan, track.view()) == true);
    }

    SECTION("Codec")
    {
      auto plan = compileOk(compiler, parseOk("@codec = FLAC"));
      CHECK(evaluator.evaluateFull(plan, track.view()) == true);
    }

    SECTION("Numeric Metadata Fields")
    {
      CHECK(evaluator.evaluateFull(compileOk(compiler, parseOk("$trackNumber = 3")), track.view()) == true);
      CHECK(evaluator.evaluateFull(compileOk(compiler, parseOk("$trackTotal = 12")), track.view()) == true);
      CHECK(evaluator.evaluateFull(compileOk(compiler, parseOk("$discNumber = 1")), track.view()) == true);
      CHECK(evaluator.evaluateFull(compileOk(compiler, parseOk("$discTotal = 2")), track.view()) == true);
    }

    SECTION("CoverArtId")
    {
      auto plan = ExecutionPlan{};
      plan.instructions.push_back(
        {.op = OpCode::LoadField, .field = static_cast<std::uint8_t>(Field::CoverArtId), .operand = 0});
      plan.instructions.push_back({.op = OpCode::LoadConstant, .operand = 1, .constValue = 99});
      plan.instructions.push_back(
        {.op = OpCode::Eq, .field = static_cast<std::uint8_t>(Field::CoverArtId), .operand = 1});
      CHECK(evaluator.evaluateFull(plan, track.view()) == true);
    }

    SECTION("TagCount")
    {
      auto plan = ExecutionPlan{};
      plan.instructions.push_back(
        {.op = OpCode::LoadField, .field = static_cast<std::uint8_t>(Field::TagCount), .operand = 0});
      plan.instructions.push_back({.op = OpCode::LoadConstant, .operand = 1, .constValue = 0});
      plan.instructions.push_back(
        {.op = OpCode::Eq, .field = static_cast<std::uint8_t>(Field::TagCount), .operand = 1});
      CHECK(evaluator.evaluateFull(plan, track.view()) == true);
    }

    SECTION("Custom Property")
    {
      auto spec2 = TrackSpec{};
      spec2.customPairs.emplace_back("customName", "customValue");
      auto track2 = TestTrack{spec2};

      auto plan = compileOk(QueryCompiler{&track2.dictionary()}, parseOk("%customName = 'customValue'"));
      CHECK(PlanEvaluator{}.evaluateFull(plan, track2.view()) == true);

      auto planManual = ExecutionPlan{};
      planManual.instructions.push_back(
        {.op = OpCode::LoadField, .field = static_cast<std::uint8_t>(Field::Custom), .operand = 0, .constValue = 0});
      planManual.stringConstants.emplace_back("");
      planManual.instructions.push_back({.op = OpCode::LoadConstant, .operand = 1, .constValue = 0});
      planManual.instructions.push_back(
        {.op = OpCode::Eq, .field = static_cast<std::uint8_t>(Field::Custom), .operand = 1, .constValue = 0});
      CHECK(PlanEvaluator{}.evaluateFull(planManual, track2.view()) == true);
    }

    SECTION("Invalid Field")
    {
      auto plan = ExecutionPlan{};
      plan.stringConstants.emplace_back("pattern");
      plan.instructions.push_back({.op = OpCode::LoadField, .field = 255, .operand = 0});
      plan.instructions.push_back({.op = OpCode::LoadConstant, .operand = 1, .constValue = 0});
      plan.instructions.push_back({.op = OpCode::Like, .operand = 1});
      CHECK(evaluator.evaluateFull(plan, track.view()) == false);
    }
  }

  TEST_CASE("PlanEvaluator compares dictionary fields lexicographically by text", "[query][unit][plan_evaluator]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto dict = DictionaryStore{openDatabase(wtxn, "dict"), wtxn};

    auto zappaId = ao::test::requireValue(dict.put(wtxn, "Zappa"));
    auto adeleId = ao::test::requireValue(dict.put(wtxn, "Adele"));
    auto mozartId = ao::test::requireValue(dict.put(wtxn, "Mozart"));
    auto kinksId = ao::test::requireValue(dict.put(wtxn, "Kinks"));

    auto compiler = QueryCompiler{&dict};
    auto evaluator = PlanEvaluator{};

    auto adeleData = makeHotOnlyTrack(adeleId);
    auto adele = library::TrackView{adeleData, std::span<std::byte const>{}};
    auto mozartData = makeHotOnlyTrack(mozartId);
    auto mozart = library::TrackView{mozartData, std::span<std::byte const>{}};
    auto zappaData = makeHotOnlyTrack(zappaId);
    auto zappa = library::TrackView{zappaData, std::span<std::byte const>{}};
    auto kinksData = makeHotOnlyTrack(kinksId);
    auto kinks = library::TrackView{kinksData, std::span<std::byte const>{}};

    SECTION("RangeMatchesByText")
    {
      auto plan = compileOk(compiler, parseOk("$artist in Adele..Mozart"));
      CHECK(evaluator.evaluateFull(plan, adele));
      CHECK(evaluator.evaluateFull(plan, kinks));
      CHECK(evaluator.evaluateFull(plan, mozart));
      CHECK_FALSE(evaluator.evaluateFull(plan, zappa));
    }

    SECTION("GreaterThanComparesByText")
    {
      auto plan = compileOk(compiler, parseOk("$artist > Mozart"));
      CHECK(evaluator.evaluateFull(plan, zappa));
      CHECK_FALSE(evaluator.evaluateFull(plan, adele));
      CHECK_FALSE(evaluator.evaluateFull(plan, mozart));
    }

    SECTION("LessThanComparesByText")
    {
      auto plan = compileOk(compiler, parseOk("$artist < Mozart"));
      CHECK(evaluator.evaluateFull(plan, adele));
      CHECK(evaluator.evaluateFull(plan, kinks));
      CHECK_FALSE(evaluator.evaluateFull(plan, mozart));
      CHECK_FALSE(evaluator.evaluateFull(plan, zappa));
    }

    SECTION("LessOrEqualComparesByText")
    {
      auto plan = compileOk(compiler, parseOk("$artist <= Mozart"));
      CHECK(evaluator.evaluateFull(plan, adele));
      CHECK(evaluator.evaluateFull(plan, mozart));
      CHECK_FALSE(evaluator.evaluateFull(plan, zappa));
    }

    SECTION("GreaterOrEqualComparesByText")
    {
      auto plan = compileOk(compiler, parseOk("$artist >= Mozart"));
      CHECK(evaluator.evaluateFull(plan, mozart));
      CHECK(evaluator.evaluateFull(plan, zappa));
      CHECK_FALSE(evaluator.evaluateFull(plan, adele));
    }

    SECTION("NotEqualStillComparesById")
    {
      auto plan = compileOk(compiler, parseOk("$artist != Mozart"));
      CHECK(evaluator.evaluateFull(plan, adele));
      CHECK(evaluator.evaluateFull(plan, zappa));
      CHECK_FALSE(evaluator.evaluateFull(plan, mozart));
    }

    SECTION("ResolvesPerFieldNotJustArtist")
    {
      auto rockData = makeHotOnlyTrack(kInvalidDictionaryId, kInvalidDictionaryId, zappaId);
      auto rock = library::TrackView{rockData, std::span<std::byte const>{}};
      auto jazzData = makeHotOnlyTrack(kInvalidDictionaryId, kInvalidDictionaryId, adeleId);
      auto jazz = library::TrackView{jazzData, std::span<std::byte const>{}};

      auto plan = compileOk(compiler, parseOk("$genre > Mozart"));
      CHECK(evaluator.evaluateFull(plan, rock));
      CHECK_FALSE(evaluator.evaluateFull(plan, jazz));
    }
  }

  TEST_CASE("PlanEvaluator matches partial LIKE patterns on dictionary-backed fields", "[query][unit][plan_evaluator]")
  {
    auto const spec = TrackSpec{.artist = "Johann Sebastian Bach"};
    auto track = TrackFixture{spec};
    auto evaluator = PlanEvaluator{};

    auto compiler = QueryCompiler{&track.dictionary()};
    auto plan = compileOk(compiler, parseOk("$artist ~ 'Bach'"));

    CHECK(evaluator.evaluateFull(plan, track.view()) == true);
  }
} // namespace ao::query::test
