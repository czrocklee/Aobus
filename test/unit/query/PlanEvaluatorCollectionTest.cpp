// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/query/ExecutionPlanTestSupport.h"
#include "test/unit/query/PlanEvaluatorTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/query/PlanEvaluator.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::query::test
{
  TEST_CASE("PlanEvaluator - matches existence predicates by field storage semantics", "[query][unit][plan-evaluator]")
  {
    auto evaluator = PlanEvaluator{};

    SECTION("StringMetadataExistsWhenNonEmpty")
    {
      auto missingSpec = TrackSpec{};
      missingSpec.title.clear();
      auto missing = TrackFixture{missingSpec};
      auto present = TrackFixture{TrackSpec{}};
      auto plan = compileOk(parseOk("$title?"));

      CHECK_FALSE(evaluator.matchesFullPlan(plan, missing.view()));
      CHECK(evaluator.matchesFullPlan(plan, present.view()));
    }

    SECTION("DictionaryMetadataExistsWhenIdIsValid")
    {
      auto missingSpec = TrackSpec{};
      missingSpec.artist.clear();
      auto missing = TrackFixture{missingSpec};
      auto present = TrackFixture{TrackSpec{}};
      auto plan = compileOk(parseOk("$artist?"));

      CHECK_FALSE(evaluator.matchesFullPlan(plan, missing.view()));
      CHECK(evaluator.matchesFullPlan(plan, present.view()));
    }

    SECTION("OtherDictionaryMetadataExistsWhenIdIsValid")
    {
      auto missingSpec = TrackSpec{};
      missingSpec.album.clear();
      missingSpec.albumArtist.clear();
      missingSpec.composer.clear();
      missingSpec.conductor.clear();
      missingSpec.ensemble.clear();
      missingSpec.work.clear();
      missingSpec.movement.clear();
      missingSpec.soloist.clear();
      missingSpec.genre.clear();
      auto missing = TrackFixture{missingSpec};

      auto presentSpec = TrackSpec{};
      presentSpec.album = "Album";
      presentSpec.albumArtist = "Album Artist";
      presentSpec.composer = "Composer";
      presentSpec.conductor = "Conductor";
      presentSpec.ensemble = "Ensemble";
      presentSpec.work = "Work";
      presentSpec.movement = "Movement";
      presentSpec.soloist = "Soloist";
      presentSpec.genre = "Genre";
      auto present = TrackFixture{presentSpec};

      for (auto const* field : {"$album?",
                                "$albumArtist?",
                                "$composer?",
                                "$conductor?",
                                "$ensemble?",
                                "$work?",
                                "$movement?",
                                "$soloist?",
                                "$genre?"})
      {
        auto plan = compileOk(parseOk(field));
        CHECK_FALSE(evaluator.matchesFullPlan(plan, missing.view()));
        CHECK(evaluator.matchesFullPlan(plan, present.view()));
      }
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

      CHECK_FALSE(evaluator.matchesFullPlan(compileOk(parseOk("$year?")), missing.view()));
      CHECK(evaluator.matchesFullPlan(compileOk(parseOk("$year?")), present.view()));
      CHECK_FALSE(evaluator.matchesFullPlan(compileOk(parseOk("$trackNumber?")), missing.view()));
      CHECK(evaluator.matchesFullPlan(compileOk(parseOk("$trackNumber?")), present.view()));
      CHECK_FALSE(evaluator.matchesFullPlan(compileOk(parseOk("$trackTotal?")), missing.view()));
      CHECK(evaluator.matchesFullPlan(compileOk(parseOk("$trackTotal?")), present.view()));
    }

    SECTION("OtherNumericMetadataExistsWhenPositive")
    {
      auto missing = TrackFixture{TrackSpec{}};

      auto presentSpec = TrackSpec{};
      presentSpec.discNumber = 1;
      presentSpec.discTotal = 2;
      presentSpec.movementNumber = 3;
      presentSpec.movementTotal = 4;
      auto present = TrackFixture{presentSpec};

      for (auto const* field : {"$discNumber?", "$discTotal?", "$movementNumber?", "$movementTotal?"})
      {
        auto plan = compileOk(parseOk(field));
        CHECK_FALSE(evaluator.matchesFullPlan(plan, missing.view()));
        CHECK(evaluator.matchesFullPlan(plan, present.view()));
      }
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

      CHECK_FALSE(evaluator.matchesFullPlan(compileOk(parseOk("@duration?")), missing.view()));
      CHECK(evaluator.matchesFullPlan(compileOk(parseOk("@duration?")), present.view()));
      CHECK_FALSE(evaluator.matchesFullPlan(compileOk(parseOk("@codec?")), missing.view()));
      CHECK(evaluator.matchesFullPlan(compileOk(parseOk("@codec?")), present.view()));
    }

    SECTION("OtherNumericPropertiesExistWhenPositive")
    {
      auto missingSpec = TrackSpec{};
      missingSpec.bitrate = 0;
      missingSpec.sampleRate = 0;
      missingSpec.channels = 0;
      missingSpec.bitDepth = 0;
      auto missing = TrackFixture{missingSpec};
      auto present = TrackFixture{TrackSpec{}};

      for (auto const* field : {"@bitrate?", "@sampleRate?", "@channels?", "@bitDepth?"})
      {
        auto plan = compileOk(parseOk(field));
        CHECK_FALSE(evaluator.matchesFullPlan(plan, missing.view()));
        CHECK(evaluator.matchesFullPlan(plan, present.view()));
      }
    }

    SECTION("CoverArtExistsWhenPrimaryResourceIsValid")
    {
      auto missing = TrackFixture{TrackSpec{}};
      auto presentSpec = TrackSpec{};
      presentSpec.coverArtId = ResourceId{42};
      auto present = TrackFixture{presentSpec};
      auto plan = compileOk(parseOk("$coverArt?"));

      CHECK_FALSE(evaluator.matchesFullPlan(plan, missing.view()));
      CHECK(evaluator.matchesFullPlan(plan, present.view()));
    }

    SECTION("CustomMetadataExistsEvenWhenValueIsEmpty")
    {
      auto absent = TrackFixture{TrackSpec{}};
      auto boundAbsentSpec = TrackSpec{};
      boundAbsentSpec.artist = "rating";
      auto boundAbsent = TrackFixture{boundAbsentSpec};
      auto emptyValueSpec = TrackSpec{};
      emptyValueSpec.customPairs.emplace_back("rating", "");
      auto emptyValue = TrackFixture{emptyValueSpec};
      auto nonEmptyValueSpec = TrackSpec{};
      nonEmptyValueSpec.customPairs.emplace_back("rating", "5");
      auto nonEmptyValue = TrackFixture{nonEmptyValueSpec};

      auto plan = compileOk(parseOk("%rating?"));

      CHECK_FALSE(matchesFullPlanWithDictionary(evaluator, plan, absent.view(), absent.dictionary()));
      CHECK_FALSE(matchesFullPlanWithDictionary(evaluator, plan, boundAbsent.view(), boundAbsent.dictionary()));
      CHECK(matchesFullPlanWithDictionary(evaluator, plan, emptyValue.view(), emptyValue.dictionary()));
      CHECK(matchesFullPlanWithDictionary(evaluator, plan, nonEmptyValue.view(), nonEmptyValue.dictionary()));
    }

    SECTION("TagExistenceMatchesMembership")
    {
      auto absent = TrackFixture{TrackSpec{}};
      auto boundAbsentSpec = TrackSpec{};
      boundAbsentSpec.artist = "favorite";
      auto boundAbsent = TrackFixture{boundAbsentSpec};
      auto presentSpec = TrackSpec{};
      presentSpec.tags.emplace_back("favorite");
      auto present = TrackFixture{presentSpec};
      auto plan = compileOk(parseOk("#favorite?"));

      CHECK_FALSE(matchesFullPlanWithDictionary(evaluator, plan, absent.view(), absent.dictionary()));
      CHECK_FALSE(matchesFullPlanWithDictionary(evaluator, plan, boundAbsent.view(), boundAbsent.dictionary()));
      CHECK(matchesFullPlanWithDictionary(evaluator, plan, present.view(), present.dictionary()));
    }

    SECTION("NegatedExistenceMatchesMissingFields")
    {
      auto missingSpec = TrackSpec{};
      missingSpec.year = 0;
      auto missing = TrackFixture{missingSpec};
      auto present = TrackFixture{TrackSpec{}};
      auto plan = compileOk(parseOk("!$year?"));

      CHECK(evaluator.matchesFullPlan(plan, missing.view()));
      CHECK_FALSE(evaluator.matchesFullPlan(plan, present.view()));
    }
  }

  TEST_CASE("PlanEvaluator - matches list membership across scalar and string fields", "[query][unit][plan-evaluator]")
  {
    auto spec = TrackSpec{};
    spec.artist = "Bach";
    spec.year = 1990;
    spec.duration = std::chrono::minutes{3};
    spec.customPairs.emplace_back("mood", "focus");
    auto track = TrackFixture{spec};

    auto evaluator = PlanEvaluator{};

    SECTION("DictionaryBackedStringMatch")
    {
      auto plan = compileOk(parseOk(R"($artist in ["Bach", "Mozart"])"));
      CHECK(matchesFullPlanWithDictionary(evaluator, plan, track.view(), track.dictionary()));
    }

    SECTION("NumericNonMatch")
    {
      auto plan = compileOk(parseOk("$year in [1988, 1989]"));
      CHECK_FALSE(evaluator.matchesFullPlan(plan, track.view()));
    }

    SECTION("UnitConstantMatch")
    {
      auto plan = compileOk(parseOk("@duration in [2m, 3m]"));
      CHECK(evaluator.matchesFullPlan(plan, track.view()));
    }

    SECTION("CustomStringMatch")
    {
      auto plan = compileOk(parseOk(R"(%mood in ["study", "focus"])"));
      CHECK(matchesFullPlanWithDictionary(evaluator, plan, track.view(), track.dictionary()));
    }

    SECTION("LargeNumericListMatch")
    {
      auto plan = compileOk(parseOk("$year in [1984, 1985, 1986, 1987, 1988, 1989, 1990, 1991]"));

      CHECK(plan.inSets.size() == 1);
      CHECK(evaluator.matchesFullPlan(plan, track.view()));
    }

    SECTION("LargeDictionaryBackedStringListMatch")
    {
      auto plan =
        compileOk(parseOk(R"($artist in ["Adams", "Bach", "Chopin", "Debussy", "Elgar", "Faure", "Glass", "Haydn"])"));

      CHECK(plan.inSets.size() == 1);
      CHECK(matchesFullPlanWithDictionary(evaluator, plan, track.view(), track.dictionary()));
    }

    SECTION("LargeCustomStringListMatch")
    {
      auto plan =
        compileOk(parseOk(R"(%mood in ["ambient", "deep", "focus", "late", "mix", "quiet", "study", "warm"])"));

      CHECK(plan.inSets.size() == 1);
      CHECK(matchesFullPlanWithDictionary(evaluator, plan, track.view(), track.dictionary()));
    }

    SECTION("LargeListNonMatch")
    {
      auto plan = compileOk(parseOk("$year in [1980, 1981, 1982, 1983, 1984, 1985, 1986, 1987]"));

      CHECK(plan.inSets.size() == 1);
      CHECK_FALSE(evaluator.matchesFullPlan(plan, track.view()));
    }
  }

  TEST_CASE("PlanEvaluator - matches inclusive ranges across numeric and duration fields",
            "[query][unit][plan-evaluator]")
  {
    auto spec = TrackSpec{};
    spec.year = 1994;
    spec.duration = std::chrono::minutes{3};
    auto track = TrackFixture{spec};

    auto evaluator = PlanEvaluator{};

    SECTION("NumericRangeMatch")
    {
      auto plan = compileOk(parseOk("$year in 1990..1999"));
      CHECK(evaluator.matchesFullPlan(plan, track.view()));

      auto lowerSpec = spec;
      lowerSpec.year = 1990;
      auto lower = TrackFixture{lowerSpec};
      CHECK(evaluator.matchesFullPlan(plan, lower.view()));

      auto upperSpec = spec;
      upperSpec.year = 1999;
      auto upper = TrackFixture{upperSpec};
      CHECK(evaluator.matchesFullPlan(plan, upper.view()));
    }

    SECTION("UnitRangeMatch")
    {
      auto plan = compileOk(parseOk("@duration in 2m30s..5m"));
      CHECK(evaluator.matchesFullPlan(plan, track.view()));

      auto lowerSpec = spec;
      lowerSpec.duration = std::chrono::minutes{2} + std::chrono::seconds{30};
      auto lower = TrackFixture{lowerSpec};
      CHECK(evaluator.matchesFullPlan(plan, lower.view()));

      auto upperSpec = spec;
      upperSpec.duration = std::chrono::minutes{5};
      auto upper = TrackFixture{upperSpec};
      CHECK(evaluator.matchesFullPlan(plan, upper.view()));
    }

    SECTION("OutOfRangeDoesNotMatch")
    {
      auto plan = compileOk(parseOk("$year in 1980..1989"));
      CHECK_FALSE(evaluator.matchesFullPlan(plan, track.view()));
    }
  }

  TEST_CASE("PlanEvaluator - matches custom metadata equality and LIKE expressions", "[query][unit][plan-evaluator]")
  {
    auto const spec = TrackSpec{.customPairs = {{"isrc", "US-RC1-12-00001"}, {"label", "Deutsche Grammophon"}}};

    auto track = TrackFixture{spec};
    auto evaluator = PlanEvaluator{};

    SECTION("Custom Field Equality Match")
    {
      auto plan = compileOk(parseOk("%isrc = 'US-RC1-12-00001'"));
      CHECK(matchesFullPlanWithDictionary(evaluator, plan, track.view(), track.dictionary()) == true);
    }

    SECTION("Custom Field Equality NonMatch")
    {
      auto plan = compileOk(parseOk("%isrc = 'UK-XYZ'"));
      CHECK(matchesFullPlanWithDictionary(evaluator, plan, track.view(), track.dictionary()) == false);
    }

    SECTION("Custom Field Like Match")
    {
      auto plan = compileOk(parseOk("%label ~ 'Grammophon'"));
      CHECK(matchesFullPlanWithDictionary(evaluator, plan, track.view(), track.dictionary()) == true);
    }

    SECTION("Custom Field Missing")
    {
      auto plan = compileOk(parseOk("%nonexistent = 'val'"));
      CHECK(matchesFullPlanWithDictionary(evaluator, plan, track.view(), track.dictionary()) == false);
    }

    SECTION("Bound Custom Field Missing")
    {
      auto boundMissingSpec = spec;
      boundMissingSpec.artist = "nonexistent";
      auto boundMissing = TrackFixture{boundMissingSpec};
      auto nonEmptyPlan = compileOk(parseOk("%nonexistent = 'val'"));

      CHECK(matchesFullPlanWithDictionary(evaluator, nonEmptyPlan, boundMissing.view(), boundMissing.dictionary()) ==
            false);
    }

    SECTION("Present Empty Custom Field Compares As Empty")
    {
      auto emptySpec = spec;
      emptySpec.customPairs.emplace_back("empty", "");
      auto empty = TrackFixture{emptySpec};
      auto emptyPlan = compileOk(parseOk("%empty = ''"));
      auto nonEmptyPlan = compileOk(parseOk("%empty = 'val'"));

      CHECK(matchesFullPlanWithDictionary(evaluator, emptyPlan, empty.view(), empty.dictionary()) == true);
      CHECK(matchesFullPlanWithDictionary(evaluator, nonEmptyPlan, empty.view(), empty.dictionary()) == false);
    }
  }
} // namespace ao::query::test
