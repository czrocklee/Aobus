// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/query/PlanEvaluatorTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/query/PlanEvaluator.h>
#include <ao/query/QueryCompiler.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::query::test
{
  TEST_CASE("PlanEvaluator matches existence predicates by field storage semantics", "[query][unit][plan_evaluator]")
  {
    auto evaluator = PlanEvaluator{};

    SECTION("StringMetadataExistsWhenNonEmpty")
    {
      auto missingSpec = TrackSpec{};
      missingSpec.title.clear();
      auto missing = TrackFixture{missingSpec};
      auto present = TrackFixture{TrackSpec{}};
      auto plan = compileOk(QueryCompiler{}, parseOk("$title?"));

      CHECK_FALSE(evaluator.evaluateFull(plan, missing.view()));
      CHECK(evaluator.evaluateFull(plan, present.view()));
    }

    SECTION("DictionaryMetadataExistsWhenIdIsValid")
    {
      auto missingSpec = TrackSpec{};
      missingSpec.artist.clear();
      auto missing = TrackFixture{missingSpec};
      auto present = TrackFixture{TrackSpec{}};
      auto plan = compileOk(QueryCompiler{}, parseOk("$artist?"));

      CHECK_FALSE(evaluator.evaluateFull(plan, missing.view()));
      CHECK(evaluator.evaluateFull(plan, present.view()));
    }

    SECTION("NumericMetadataExistsWhenPositive")
    {
      auto missingSpec = TrackSpec{};
      missingSpec.year = 0;
      missingSpec.trackNumber = 0;
      missingSpec.trackTotal = 0;
      auto missing = TrackFixture{missingSpec};

      auto presentSpec = TrackSpec{};
      presentSpec.year = 2024;
      presentSpec.trackNumber = 3;
      presentSpec.trackTotal = 12;
      auto present = TrackFixture{presentSpec};

      CHECK_FALSE(evaluator.evaluateFull(compileOk(QueryCompiler{}, parseOk("$year?")), missing.view()));
      CHECK(evaluator.evaluateFull(compileOk(QueryCompiler{}, parseOk("$year?")), present.view()));
      CHECK_FALSE(evaluator.evaluateFull(compileOk(QueryCompiler{}, parseOk("$trackNumber?")), missing.view()));
      CHECK(evaluator.evaluateFull(compileOk(QueryCompiler{}, parseOk("$trackNumber?")), present.view()));
      CHECK_FALSE(evaluator.evaluateFull(compileOk(QueryCompiler{}, parseOk("$trackTotal?")), missing.view()));
      CHECK(evaluator.evaluateFull(compileOk(QueryCompiler{}, parseOk("$trackTotal?")), present.view()));
    }

    SECTION("PropertiesExistWhenPositiveOrKnown")
    {
      auto missingSpec = TrackSpec{};
      missingSpec.duration = std::chrono::milliseconds{0};
      missingSpec.codec = AudioCodec::Unknown;
      auto missing = TrackFixture{missingSpec};

      auto presentSpec = TrackSpec{};
      presentSpec.duration = std::chrono::milliseconds{1};
      presentSpec.codec = AudioCodec::Flac;
      auto present = TrackFixture{presentSpec};

      CHECK_FALSE(evaluator.evaluateFull(compileOk(QueryCompiler{}, parseOk("@duration?")), missing.view()));
      CHECK(evaluator.evaluateFull(compileOk(QueryCompiler{}, parseOk("@duration?")), present.view()));
      CHECK_FALSE(evaluator.evaluateFull(compileOk(QueryCompiler{}, parseOk("@codec?")), missing.view()));
      CHECK(evaluator.evaluateFull(compileOk(QueryCompiler{}, parseOk("@codec?")), present.view()));
    }

    SECTION("CoverArtExistsWhenPrimaryResourceIsValid")
    {
      auto missing = TrackFixture{TrackSpec{}};
      auto presentSpec = TrackSpec{};
      presentSpec.coverArtId = ResourceId{42};
      auto present = TrackFixture{presentSpec};
      auto plan = compileOk(QueryCompiler{}, parseOk("$coverArt?"));

      CHECK_FALSE(evaluator.evaluateFull(plan, missing.view()));
      CHECK(evaluator.evaluateFull(plan, present.view()));
    }

    SECTION("CustomMetadataExistsEvenWhenValueIsEmpty")
    {
      auto absent = TrackFixture{TrackSpec{}};
      auto emptyValueSpec = TrackSpec{};
      emptyValueSpec.customPairs.emplace_back("rating", "");
      auto emptyValue = TrackFixture{emptyValueSpec};
      auto nonEmptyValueSpec = TrackSpec{};
      nonEmptyValueSpec.customPairs.emplace_back("rating", "5");
      auto nonEmptyValue = TrackFixture{nonEmptyValueSpec};

      auto plan = compileOk(QueryCompiler{&emptyValue.dictionary()}, parseOk("%rating?"));

      CHECK_FALSE(evaluator.evaluateFull(plan, absent.view()));
      CHECK(evaluator.evaluateFull(plan, emptyValue.view()));
      CHECK(evaluator.evaluateFull(plan, nonEmptyValue.view()));
    }

    SECTION("TagExistenceMatchesMembership")
    {
      auto absent = TrackFixture{TrackSpec{}};
      auto presentSpec = TrackSpec{};
      presentSpec.tags.emplace_back("favorite");
      auto present = TrackFixture{presentSpec};
      auto plan = compileOk(QueryCompiler{&present.dictionary()}, parseOk("#favorite?"));

      CHECK_FALSE(evaluator.evaluateFull(plan, absent.view()));
      CHECK(evaluator.evaluateFull(plan, present.view()));
    }

    SECTION("NegatedExistenceMatchesMissingFields")
    {
      auto missingSpec = TrackSpec{};
      missingSpec.year = 0;
      auto missing = TrackFixture{missingSpec};
      auto present = TrackFixture{TrackSpec{}};
      auto plan = compileOk(QueryCompiler{}, parseOk("!$year?"));

      CHECK(evaluator.evaluateFull(plan, missing.view()));
      CHECK_FALSE(evaluator.evaluateFull(plan, present.view()));
    }
  }

  TEST_CASE("PlanEvaluator matches list membership across scalar and string fields", "[query][unit][plan_evaluator]")
  {
    auto spec = TrackSpec{};
    spec.artist = "Bach";
    spec.year = 1990;
    spec.duration = std::chrono::minutes{3};
    spec.customPairs.emplace_back("mood", "focus");
    auto track = TrackFixture{spec};

    auto evaluator = PlanEvaluator{};
    auto compiler = QueryCompiler{&track.dictionary()};

    SECTION("DictionaryBackedStringMatch")
    {
      auto plan = compileOk(compiler, parseOk(R"($artist in ["Bach", "Mozart"])"));
      CHECK(evaluator.evaluateFull(plan, track.view()));
    }

    SECTION("NumericNonMatch")
    {
      auto plan = compileOk(compiler, parseOk("$year in [1988, 1989]"));
      CHECK_FALSE(evaluator.evaluateFull(plan, track.view()));
    }

    SECTION("UnitConstantMatch")
    {
      auto plan = compileOk(compiler, parseOk("@duration in [2m, 3m]"));
      CHECK(evaluator.evaluateFull(plan, track.view()));
    }

    SECTION("CustomStringMatch")
    {
      auto plan = compileOk(compiler, parseOk(R"(%mood in ["study", "focus"])"));
      CHECK(evaluator.evaluateFull(plan, track.view()));
    }

    SECTION("LargeNumericListMatch")
    {
      auto plan = compileOk(compiler, parseOk("$year in [1984, 1985, 1986, 1987, 1988, 1989, 1990, 1991]"));

      CHECK(plan.inSets.size() == 1);
      CHECK(evaluator.evaluateFull(plan, track.view()));
    }

    SECTION("LargeDictionaryBackedStringListMatch")
    {
      auto plan = compileOk(
        compiler, parseOk(R"($artist in ["Adams", "Bach", "Chopin", "Debussy", "Elgar", "Faure", "Glass", "Haydn"])"));

      CHECK(plan.inSets.size() == 1);
      CHECK(evaluator.evaluateFull(plan, track.view()));
    }

    SECTION("LargeCustomStringListMatch")
    {
      auto plan = compileOk(
        compiler, parseOk(R"(%mood in ["ambient", "deep", "focus", "late", "mix", "quiet", "study", "warm"])"));

      CHECK(plan.inSets.size() == 1);
      CHECK(evaluator.evaluateFull(plan, track.view()));
    }

    SECTION("LargeListNonMatch")
    {
      auto plan = compileOk(compiler, parseOk("$year in [1980, 1981, 1982, 1983, 1984, 1985, 1986, 1987]"));

      CHECK(plan.inSets.size() == 1);
      CHECK_FALSE(evaluator.evaluateFull(plan, track.view()));
    }
  }

  TEST_CASE("PlanEvaluator matches inclusive ranges across numeric and duration fields",
            "[query][unit][plan_evaluator]")
  {
    auto spec = TrackSpec{};
    spec.year = 1994;
    spec.duration = std::chrono::minutes{3};
    auto track = TrackFixture{spec};

    auto evaluator = PlanEvaluator{};
    auto compiler = QueryCompiler{&track.dictionary()};

    SECTION("NumericRangeMatch")
    {
      auto plan = compileOk(compiler, parseOk("$year in 1990..1999"));
      CHECK(evaluator.evaluateFull(plan, track.view()));
    }

    SECTION("UnitRangeMatch")
    {
      auto plan = compileOk(compiler, parseOk("@duration in 2m30s..5m"));
      CHECK(evaluator.evaluateFull(plan, track.view()));
    }

    SECTION("OutOfRangeDoesNotMatch")
    {
      auto plan = compileOk(compiler, parseOk("$year in 1980..1989"));
      CHECK_FALSE(evaluator.evaluateFull(plan, track.view()));
    }
  }

  TEST_CASE("PlanEvaluator matches custom metadata equality and LIKE expressions", "[query][unit][plan_evaluator]")
  {
    auto const spec = TrackSpec{.customPairs = {{"isrc", "US-RC1-12-00001"}, {"label", "Deutsche Grammophon"}}};

    auto track = TrackFixture{spec};
    auto evaluator = PlanEvaluator{};
    auto compiler = QueryCompiler{&track.dictionary()};

    SECTION("Custom Field Equality Match")
    {
      auto plan = compileOk(compiler, parseOk("%isrc = 'US-RC1-12-00001'"));
      CHECK(evaluator.evaluateFull(plan, track.view()) == true);
    }

    SECTION("Custom Field Equality NonMatch")
    {
      auto plan = compileOk(compiler, parseOk("%isrc = 'UK-XYZ'"));
      CHECK(evaluator.evaluateFull(plan, track.view()) == false);
    }

    SECTION("Custom Field Like Match")
    {
      auto plan = compileOk(compiler, parseOk("%label ~ 'Grammophon'"));
      CHECK(evaluator.evaluateFull(plan, track.view()) == true);
    }

    SECTION("Custom Field Missing")
    {
      auto plan = compileOk(compiler, parseOk("%nonexistent = 'val'"));
      CHECK(evaluator.evaluateFull(plan, track.view()) == false);
    }
  }
} // namespace ao::query::test
