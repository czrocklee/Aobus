// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/query/PlanEvaluatorTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/TrackView.h>
#include <ao/query/Field.h>
#include <ao/query/PlanEvaluator.h>
#include <ao/query/QueryCompiler.h>
#include <ao/query/detail/Bytecode.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ao::query::test
{
  TEST_CASE("PlanEvaluator - matches work metadata equality and LIKE expressions", "[query][unit][plan-evaluator]")
  {
    auto trackWithWork = TestTrack{
      "Title", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {}, "", "Symphony No. 5"};
    auto trackWithoutWork =
      TestTrack{"Title", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {}, "", ""};
    auto evaluator = PlanEvaluator{};

    SECTION("$work Equality")
    {
      auto expr = parseOk("$work = 'Symphony No. 5'");
      auto compiler = QueryCompiler{};
      auto plan = compileOk(compiler, expr);
      CHECK(evaluateWithDictionary(evaluator, plan, trackWithWork.view(), trackWithWork.dictionary()) == true);
      CHECK(evaluateWithDictionary(evaluator, plan, trackWithoutWork.view(), trackWithoutWork.dictionary()) == false);
    }

    SECTION("$w Equality (shorthand)")
    {
      auto expr = parseOk("$w = 'Symphony No. 5'");
      auto compiler = QueryCompiler{};
      auto plan = compileOk(compiler, expr);
      CHECK(evaluateWithDictionary(evaluator, plan, trackWithWork.view(), trackWithWork.dictionary()) == true);
    }

    SECTION("$work LIKE")
    {
      auto expr = parseOk("$work ~ Symphony");
      auto compiler = QueryCompiler{};
      auto plan = compileOk(compiler, expr);
      CHECK(evaluateWithDictionary(evaluator, plan, trackWithWork.view(), trackWithWork.dictionary()) == true);
      CHECK(evaluateWithDictionary(evaluator, plan, trackWithoutWork.view(), trackWithoutWork.dictionary()) == false);
    }
  }

  TEST_CASE("PlanEvaluator - matches movement metadata equality existence and LIKE expressions",
            "[query][unit][plan-evaluator]")
  {
    auto specWithMovement = TrackSpec{};
    specWithMovement.movement = "Finale";
    specWithMovement.movementNumber = 4;
    specWithMovement.movementTotal = 4;
    auto trackWithMovement = TestTrack{specWithMovement};

    auto specWithoutMovement = TrackSpec{};
    specWithoutMovement.movement = "";
    specWithoutMovement.movementNumber = 0;
    specWithoutMovement.movementTotal = 0;
    auto trackWithoutMovement = TestTrack{specWithoutMovement};

    auto evaluator = PlanEvaluator{};

    SECTION("$movement Equality")
    {
      auto expr = parseOk("$movement = Finale");
      auto compiler = QueryCompiler{};
      auto plan = compileOk(compiler, expr);
      CHECK(evaluateWithDictionary(evaluator, plan, trackWithMovement.view(), trackWithMovement.dictionary()) == true);
      CHECK(evaluateWithDictionary(evaluator, plan, trackWithoutMovement.view(), trackWithoutMovement.dictionary()) ==
            false);
    }

    SECTION("$m LIKE")
    {
      auto expr = parseOk("$m ~ Fin");
      auto compiler = QueryCompiler{};
      auto plan = compileOk(compiler, expr);
      CHECK(evaluateWithDictionary(evaluator, plan, trackWithMovement.view(), trackWithMovement.dictionary()) == true);
    }

    SECTION("Movement number existence")
    {
      auto expr = parseOk("$movementNumber? and $movementTotal?");
      auto compiler = QueryCompiler{};
      auto plan = compileOk(compiler, expr);
      CHECK(evaluator.evaluateFull(plan, trackWithMovement.view()) == true);
      CHECK(evaluator.evaluateFull(plan, trackWithoutMovement.view()) == false);
    }
  }

  TEST_CASE("PlanEvaluator - matches composer metadata equality and LIKE expressions", "[query][unit][plan-evaluator]")
  {
    auto trackWithComposer = TestTrack{
      "Title", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {}, "Beethoven", ""};
    auto evaluator = PlanEvaluator{};

    SECTION("$composer Equality")
    {
      auto expr = parseOk("$composer = Beethoven");
      auto compiler = QueryCompiler{};
      auto plan = compileOk(compiler, expr);
      CHECK(evaluateWithDictionary(evaluator, plan, trackWithComposer.view(), trackWithComposer.dictionary()) == true);
    }

    SECTION("$composer LIKE")
    {
      auto expr = parseOk("$composer ~ Beet");
      auto compiler = QueryCompiler{};
      auto plan = compileOk(compiler, expr);
      CHECK(evaluateWithDictionary(evaluator, plan, trackWithComposer.view(), trackWithComposer.dictionary()) == true);
    }
  }

  TEST_CASE("PlanEvaluator - dictionary cache preserves string predicate results", "[query][unit][plan-evaluator]")
  {
    auto dictionaryFixture = DictionaryFixture{};
    auto transaction = dictionaryFixture.writeTransaction();
    auto bachId = ao::test::requireValue(transaction.dictionary().intern("Johann Sebastian Bach"));
    auto mozartId = ao::test::requireValue(transaction.dictionary().intern("Wolfgang Amadeus Mozart"));
    REQUIRE(transaction.commit());
    auto const& dictionary = dictionaryFixture.dictionary();

    auto compiler = QueryCompiler{};
    auto evaluator = PlanEvaluator{};

    auto matchingHotData = makeHotOnlyTrack(bachId);
    auto matchingTrack = library::TrackView{matchingHotData, std::span<std::byte const>{}};
    auto nonMatchingHotData = makeHotOnlyTrack(mozartId);
    auto nonMatchingTrack = library::TrackView{nonMatchingHotData, std::span<std::byte const>{}};
    auto missingArtistHotData = makeHotOnlyTrack();
    auto missingArtistTrack = library::TrackView{missingArtistHotData, std::span<std::byte const>{}};

    auto dictionaryCache = library::DictionaryReadCache{dictionary};
    auto dictionaryContext = library::DictionaryReadContext{dictionaryCache};

    struct PredicateCase final
    {
      std::string_view expression;
      bool missingArtistMatches;
    };

    auto const cases = std::array{
      PredicateCase{.expression = R"($artist ~ "Bach")", .missingArtistMatches = false},
      PredicateCase{.expression = R"($artist < "K")", .missingArtistMatches = true},
      PredicateCase{.expression = R"($artist in ["Johann Sebastian Bach", "George Frideric Handel"])",
                    .missingArtistMatches = false},
    };

    for (auto const& testCase : cases)
    {
      DYNAMIC_SECTION(testCase.expression)
      {
        auto const plan = compileOk(compiler, parseOk(testCase.expression));
        auto const binding = PlanBinding{plan, dictionaryContext};

        CHECK(evaluator.evaluateFull(binding, matchingTrack) == true);
        CHECK(evaluator.evaluateFull(binding, nonMatchingTrack) == false);
        CHECK(evaluator.evaluateFull(binding, missingArtistTrack) == testCase.missingArtistMatches);
      }
    }
  }

  TEST_CASE("PlanEvaluator - nested comparison does not leak a dictionary constant into its parent",
            "[query][unit][plan-evaluator][regression]")
  {
    auto dictionaryFixture = DictionaryFixture{};
    auto transaction = dictionaryFixture.writeTransaction();
    auto const bachId = ao::test::requireValue(transaction.dictionary().intern("Bach"));
    auto const otherId = ao::test::requireValue(transaction.dictionary().intern("Other"));
    REQUIRE(transaction.commit());

    auto spec = TrackSpec{};
    spec.albumId = bachId.raw();
    spec.artistId = otherId.raw();
    auto track = TestTrack{spec, &dictionaryFixture.dictionary()};
    auto const plan = compileOk(QueryCompiler{}, parseOk(R"($album = ($artist = "Bach"))"));

    CHECK(evaluateWithDictionary(PlanEvaluator{}, plan, track.view(), dictionaryFixture.dictionary()) == false);
  }

  TEST_CASE("PlanEvaluator - matches dictionary-backed metadata and property fields", "[query][unit][plan-evaluator]")
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
    spec.conductor = "Test Conductor";
    spec.ensemble = "Test Ensemble";
    spec.soloist = "Test Soloist";
    auto track = TestTrack{spec};

    auto const& dictionary = track.dictionary();
    auto compiler = QueryCompiler{};
    auto evaluator = PlanEvaluator{};

    SECTION("Album")
    {
      auto plan = compileOk(compiler, parseOk("$album = 'Test Album'"));
      CHECK(evaluateWithDictionary(evaluator, plan, track.view(), dictionary) == true);

      auto planLike = compileOk(compiler, parseOk("$album ~ 'Test'"));
      CHECK(evaluateWithDictionary(evaluator, planLike, track.view(), dictionary) == true);
    }

    SECTION("Genre")
    {
      auto plan = compileOk(compiler, parseOk("$genre = 'Test Genre'"));
      CHECK(evaluateWithDictionary(evaluator, plan, track.view(), dictionary) == true);

      auto planLike = compileOk(compiler, parseOk("$genre ~ 'Genre'"));
      CHECK(evaluateWithDictionary(evaluator, planLike, track.view(), dictionary) == true);
    }

    SECTION("AlbumArtist")
    {
      auto plan = compileOk(compiler, parseOk("$albumArtist = 'Test Album Artist'"));
      CHECK(evaluateWithDictionary(evaluator, plan, track.view(), dictionary) == true);

      auto planLike = compileOk(compiler, parseOk("$albumArtist ~ 'Album Artist'"));
      CHECK(evaluateWithDictionary(evaluator, planLike, track.view(), dictionary) == true);
    }

    SECTION("Classical role fields")
    {
      CHECK(evaluateWithDictionary(
        evaluator, compileOk(compiler, parseOk("$conductor = 'Test Conductor'")), track.view(), dictionary));
      CHECK(evaluateWithDictionary(
        evaluator, compileOk(compiler, parseOk("$ensemble ~ 'Ensemble'")), track.view(), dictionary));
      CHECK(evaluateWithDictionary(
        evaluator, compileOk(compiler, parseOk("$soloist = 'Test Soloist'")), track.view(), dictionary));
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

    SECTION("Custom Metadata")
    {
      auto spec2 = TrackSpec{};
      spec2.customPairs.emplace_back("customName", "customValue");
      auto track2 = TestTrack{spec2};

      auto plan = compileOk(QueryCompiler{}, parseOk("%customName = 'customValue'"));
      CHECK(evaluateWithDictionary(PlanEvaluator{}, plan, track2.view(), track2.dictionary()) == true);

      auto planManual = ExecutionPlan{};
      planManual.instructions.push_back(
        {.op = OpCode::LoadField, .field = static_cast<std::uint8_t>(Field::Custom), .operand = 0, .constValue = 0});
      planManual.stringConstants.emplace_back("");
      planManual.instructions.push_back({.op = OpCode::LoadConstant, .operand = 1, .constValue = 0});
      planManual.instructions.push_back(
        {.op = OpCode::Eq, .field = static_cast<std::uint8_t>(Field::Custom), .operand = 1, .constValue = 0});
      // Bytecode without a bound custom-key symbol represents an unresolved key.
      CHECK_FALSE(PlanEvaluator{}.evaluateFull(planManual, track2.view()));
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

  TEST_CASE("PlanEvaluator - compares dictionary fields lexicographically by text", "[query][unit][plan-evaluator]")
  {
    auto dictionaryFixture = DictionaryFixture{};
    auto transaction = dictionaryFixture.writeTransaction();
    auto zappaId = ao::test::requireValue(transaction.dictionary().intern("Zappa"));
    auto adeleId = ao::test::requireValue(transaction.dictionary().intern("Adele"));
    auto mozartId = ao::test::requireValue(transaction.dictionary().intern("Mozart"));
    auto kinksId = ao::test::requireValue(transaction.dictionary().intern("Kinks"));
    REQUIRE(transaction.commit());
    auto const& dictionary = dictionaryFixture.dictionary();

    auto compiler = QueryCompiler{};
    auto evaluator = PlanEvaluator{};
    auto evaluate = [&](ExecutionPlan const& plan, library::TrackView const& track)
    { return evaluateWithDictionary(evaluator, plan, track, dictionary); };

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
      CHECK(evaluate(plan, adele));
      CHECK(evaluate(plan, kinks));
      CHECK(evaluate(plan, mozart));
      CHECK_FALSE(evaluate(plan, zappa));
    }

    SECTION("GreaterThanComparesByText")
    {
      auto plan = compileOk(compiler, parseOk("$artist > Mozart"));
      CHECK(evaluate(plan, zappa));
      CHECK_FALSE(evaluate(plan, adele));
      CHECK_FALSE(evaluate(plan, mozart));
    }

    SECTION("LessThanComparesByText")
    {
      auto plan = compileOk(compiler, parseOk("$artist < Mozart"));
      CHECK(evaluate(plan, adele));
      CHECK(evaluate(plan, kinks));
      CHECK_FALSE(evaluate(plan, mozart));
      CHECK_FALSE(evaluate(plan, zappa));
    }

    SECTION("LessOrEqualComparesByText")
    {
      auto plan = compileOk(compiler, parseOk("$artist <= Mozart"));
      CHECK(evaluate(plan, adele));
      CHECK(evaluate(plan, mozart));
      CHECK_FALSE(evaluate(plan, zappa));
    }

    SECTION("GreaterOrEqualComparesByText")
    {
      auto plan = compileOk(compiler, parseOk("$artist >= Mozart"));
      CHECK(evaluate(plan, mozart));
      CHECK(evaluate(plan, zappa));
      CHECK_FALSE(evaluate(plan, adele));
    }

    SECTION("NotEqualStillComparesById")
    {
      auto plan = compileOk(compiler, parseOk("$artist != Mozart"));
      CHECK(evaluate(plan, adele));
      CHECK(evaluate(plan, zappa));
      CHECK_FALSE(evaluate(plan, mozart));
    }

    SECTION("ResolvesPerFieldNotJustArtist")
    {
      auto rockData = makeHotOnlyTrack(kInvalidDictionaryId, kInvalidDictionaryId, zappaId);
      auto rock = library::TrackView{rockData, std::span<std::byte const>{}};
      auto jazzData = makeHotOnlyTrack(kInvalidDictionaryId, kInvalidDictionaryId, adeleId);
      auto jazz = library::TrackView{jazzData, std::span<std::byte const>{}};

      auto plan = compileOk(compiler, parseOk("$genre > Mozart"));
      CHECK(evaluate(plan, rock));
      CHECK_FALSE(evaluate(plan, jazz));
    }
  }

  TEST_CASE("PlanEvaluator - matches partial LIKE patterns on dictionary-backed fields",
            "[query][unit][plan-evaluator]")
  {
    auto const spec = TrackSpec{.artist = "Johann Sebastian Bach"};
    auto track = TrackFixture{spec};
    auto evaluator = PlanEvaluator{};

    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, parseOk("$artist ~ 'Bach'"));

    CHECK(evaluateWithDictionary(evaluator, plan, track.view(), track.dictionary()) == true);
  }

  TEST_CASE("PlanEvaluator - unresolved comparison symbols have explicit missing semantics",
            "[query][unit][plan-evaluator]")
  {
    auto track = TrackFixture{};
    auto const evaluator = PlanEvaluator{};

    auto const equalPlan = compileOk(QueryCompiler{}, parseOk("$artist = 'never interned'"));
    auto const notEqualPlan = compileOk(QueryCompiler{}, parseOk("$artist != 'never interned'"));
    CHECK_FALSE(evaluateWithDictionary(evaluator, equalPlan, track.view(), track.dictionary()));
    CHECK(evaluateWithDictionary(evaluator, notEqualPlan, track.view(), track.dictionary()));

    auto const customEqual = compileOk(QueryCompiler{}, parseOk("%missing = ''"));
    auto const customNotEqual = compileOk(QueryCompiler{}, parseOk("%missing != ''"));
    auto const customLike = compileOk(QueryCompiler{}, parseOk("%missing ~ ''"));
    auto const customIn = compileOk(QueryCompiler{}, parseOk("%missing in ['']"));
    CHECK_FALSE(evaluateWithDictionary(evaluator, customEqual, track.view(), track.dictionary()));
    CHECK(evaluateWithDictionary(evaluator, customNotEqual, track.view(), track.dictionary()));
    CHECK_FALSE(evaluateWithDictionary(evaluator, customLike, track.view(), track.dictionary()));
    CHECK_FALSE(evaluateWithDictionary(evaluator, customIn, track.view(), track.dictionary()));
  }

  TEST_CASE("PlanEvaluator - dictionary literal binding does not leak into later field comparisons",
            "[query][unit][plan-evaluator]")
  {
    auto dictionaryFixture = DictionaryFixture{};
    auto transaction = dictionaryFixture.writeTransaction();
    auto const firstId = ao::test::requireValue(transaction.dictionary().intern("first"));
    auto const secondId = ao::test::requireValue(transaction.dictionary().intern("second"));
    REQUIRE(transaction.commit());

    auto data = makeHotOnlyTrack(firstId, secondId, secondId);
    auto track = library::TrackView{data, std::span<std::byte const>{}};
    auto const plan = compileOk(QueryCompiler{}, parseOk("$artist = 'first' and $album = $genre"));

    CHECK(evaluateWithDictionary(PlanEvaluator{}, plan, track, dictionaryFixture.dictionary()));
  }
} // namespace ao::query::test
