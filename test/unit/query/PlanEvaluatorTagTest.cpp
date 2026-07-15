// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/library/LibraryBinaryTestSupport.h"
#include "test/unit/query/PlanEvaluatorTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>
#include <ao/query/Field.h>
#include <ao/query/PlanEvaluator.h>
#include <ao/query/detail/Bytecode.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <vector>

namespace ao::query::test
{
  TEST_CASE("PlanEvaluator - keeps OR candidates when only one branch requires a tag", "[query][unit][plan-evaluator]")
  {
    auto dictionaryFixture = DictionaryFixture{};
    auto const aimerId = dictionaryFixture.intern("Aimer");
    auto const& dictionary = dictionaryFixture.dictionary();

    auto const plan = compileOk(QueryCompiler{}, parseOk(R"($artist ~ "Aimer" or #Aimer)"));
    CHECK(plan.requiredTagSymbols.empty());

    auto cache = library::DictionaryReadCache{dictionary};
    auto context = library::DictionaryReadContext{cache};
    auto const binding = PlanBinding{plan, context};
    auto const evaluator = PlanEvaluator{};

    auto artistMatchData = makeHotOnlyTrack(aimerId);
    auto artistMatch = library::TrackView{artistMatchData, std::span<std::byte const>{}};
    CHECK(evaluator.matches(binding, artistMatch));

    auto const tagIds = std::array{aimerId};
    auto tagMatchData =
      makeHotOnlyTrack(kInvalidDictionaryId, kInvalidDictionaryId, kInvalidDictionaryId, kInvalidDictionaryId, tagIds);
    auto tagMatch = library::TrackView{tagMatchData, std::span<std::byte const>{}};
    CHECK(evaluator.matches(binding, tagMatch));

    auto noMatchData = makeHotOnlyTrack();
    auto noMatch = library::TrackView{noMatchData, std::span<std::byte const>{}};
    CHECK_FALSE(evaluator.matches(binding, noMatch));
  }

  TEST_CASE("PlanEvaluator - unresolved tag symbols are false and do not mutate the dictionary",
            "[query][unit][plan-evaluator]")
  {
    auto dictionaryFixture = DictionaryFixture{};
    auto const& dictionary = dictionaryFixture.dictionary();
    auto const plan = compileOk(QueryCompiler{}, parseOk("#future"));
    auto noTagsData = makeHotOnlyTrack();
    auto noTags = library::TrackView{noTagsData, std::span<std::byte const>{}};

    CHECK_FALSE(matchesWithDictionary(PlanEvaluator{}, plan, noTags, dictionary));
    CHECK(dictionary.size() == 0);
    CHECK_FALSE(dictionary.findId("future"));
  }

  TEST_CASE("PlanEvaluator - matches present tags and rejects absent tags", "[query][unit][plan-evaluator]")
  {
    auto present = TrackFixture{TrackSpec{.tags = {"rock"}}};
    auto absent = TrackFixture{TrackSpec{.tags = {"jazz"}}};
    auto const plan = compileOk(QueryCompiler{}, parseOk("#rock"));
    auto const evaluator = PlanEvaluator{};

    CHECK(matchesWithDictionary(evaluator, plan, present.view(), present.dictionary()));
    CHECK_FALSE(matchesWithDictionary(evaluator, plan, absent.view(), absent.dictionary()));
  }

  TEST_CASE("PlanEvaluator - matches numeric tag names and quoted custom keys", "[query][unit][plan-evaluator]")
  {
    auto spec = TrackSpec{};
    spec.tags.emplace_back("123");
    spec.customPairs.emplace_back("Replay Gain", "high");
    auto track = TrackFixture{spec};
    auto const plan = compileOk(QueryCompiler{}, parseOk(R"(#123 and %"Replay Gain" = "high")"));

    CHECK(evaluateWithDictionary(PlanEvaluator{}, plan, track.view(), track.dictionary()));
  }

  TEST_CASE("PlanEvaluator - compiles tag fields into bindable field loads", "[query][unit][plan-evaluator]")
  {
    auto const plan = compileOk(QueryCompiler{}, parseOk("#tagname"));

    REQUIRE(plan.instructions.size() >= 3);
    CHECK(plan.instructions[0].op == OpCode::LoadField);
    CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(Field::Tag));
    CHECK(plan.instructions.back().dictionarySymbol == 0);
    CHECK(plan.dictionarySymbols == std::vector<std::string>{"tagname"});
  }

  TEST_CASE("PlanEvaluator - reads track tag bloom bits from hot data", "[query][unit][plan-evaluator]")
  {
    {
      auto header = library::TrackHotHeader{};
      header.tagBloom = (1U << (10 & 31));
      auto data = serializeHeader(header);
      data.push_back(static_cast<std::byte>('\0'));
      auto view = library::TrackView{data, std::span<std::byte const>{}};
      CHECK(view.tags().bloom() == (1U << 10));
    }

    {
      auto header = library::TrackHotHeader{};
      header.tagBloom = (1U << (32 & 31));
      auto data = serializeHeader(header);
      data.push_back(static_cast<std::byte>('\0'));
      auto view = library::TrackView{data, std::span<std::byte const>{}};
      CHECK(view.tags().bloom() == 1U);
    }

    {
      auto header = library::TrackHotHeader{};
      header.tagBloom = (1U << (5 & 31)) | (1U << (20 & 31));
      auto data = serializeHeader(header);
      data.push_back(static_cast<std::byte>('\0'));
      auto view = library::TrackView{data, std::span<std::byte const>{}};
      CHECK((view.tags().bloom() & (1U << 5)) != 0);
      CHECK((view.tags().bloom() & (1U << 20)) != 0);
    }
  }

  TEST_CASE("PlanEvaluator - verifies exact membership after a tag bloom collision", "[query][unit][plan-evaluator]")
  {
    auto dictionaryFixture = DictionaryFixture{};
    auto transaction = dictionaryFixture.writeTransaction();
    auto const targetId = ao::test::requireValue(transaction.dictionary().intern("target"));

    for (std::int32_t index = 0; index < 31; ++index)
    {
      auto const filler = std::format("filler-{}", index);
      REQUIRE(transaction.dictionary().intern(filler));
    }

    auto const collisionId = ao::test::requireValue(transaction.dictionary().intern("collision"));
    REQUIRE(transaction.commit());
    REQUIRE(targetId != collisionId);
    REQUIRE((targetId.raw() & 31U) == (collisionId.raw() & 31U));

    auto const tagIds = std::array{collisionId};
    auto collisionData =
      makeHotOnlyTrack(kInvalidDictionaryId, kInvalidDictionaryId, kInvalidDictionaryId, kInvalidDictionaryId, tagIds);
    auto collisionTrack = library::TrackView{collisionData, std::span<std::byte const>{}};
    auto const plan = compileOk(QueryCompiler{}, parseOk("#target"));

    CHECK_FALSE(matchesWithDictionary(PlanEvaluator{}, plan, collisionTrack, dictionaryFixture.dictionary()));
  }

  TEST_CASE("PlanEvaluator - tag bloom rejects before full bytecode evaluation",
            "[query][unit][plan-evaluator][regression]")
  {
    auto dictionaryFixture = DictionaryFixture{};
    auto const requiredId = dictionaryFixture.intern("required");
    REQUIRE(requiredId != kInvalidDictionaryId);
    auto plan = ExecutionPlan{
      .instructions = {{.op = OpCode::LoadConstant, .operand = 0, .constValue = 1}},
      .stringConstants = {},
      .inSets = {},
      .dictionarySymbols = {"required"},
      .requiredTagSymbols = {0},
      .matchesAll = false,
      .requiresDictionary = true,
      .accessProfile = AccessProfile::HotOnly,
    };
    auto context = library::DictionaryReadContext{dictionaryFixture.dictionary()};
    auto const binding = PlanBinding{plan, context};
    auto data = makeHotOnlyTrack();
    auto track = library::TrackView{data, std::span<std::byte const>{}};
    auto const evaluator = PlanEvaluator{};

    CHECK(evaluator.evaluateFull(binding, track));
    CHECK_FALSE(evaluator.matches(binding, track));
  }

  TEST_CASE("PlanEvaluator - requires every tag in an AND expression", "[query][unit][plan-evaluator]")
  {
    auto allTags = TrackFixture{TrackSpec{.tags = {"rock", "jazz", "blues"}}};
    auto rockOnly = TrackFixture{TrackSpec{.tags = {"rock"}}};
    auto const plan = compileOk(QueryCompiler{}, parseOk("#rock and #jazz"));
    auto const evaluator = PlanEvaluator{};

    CHECK(matchesWithDictionary(evaluator, plan, allTags.view(), allTags.dictionary()));
    CHECK_FALSE(matchesWithDictionary(evaluator, plan, rockOnly.view(), rockOnly.dictionary()));
  }

  TEST_CASE("PlanEvaluator - a new dictionary generation requires a new binding", "[query][unit][plan-evaluator]")
  {
    auto dictionaryFixture = DictionaryFixture{};
    auto const& dictionary = dictionaryFixture.dictionary();
    auto const plan = compileOk(QueryCompiler{}, parseOk("#future"));
    auto oldContext = library::DictionaryReadContext{dictionary};
    auto const oldBinding = PlanBinding{plan, oldContext};

    auto transaction = dictionaryFixture.writeTransaction();
    auto const futureId = ao::test::requireValue(transaction.dictionary().intern("future"));
    REQUIRE(transaction.commit());

    auto const tagIds = std::array{futureId};
    auto data =
      makeHotOnlyTrack(kInvalidDictionaryId, kInvalidDictionaryId, kInvalidDictionaryId, kInvalidDictionaryId, tagIds);
    auto track = library::TrackView{data, std::span<std::byte const>{}};
    auto const evaluator = PlanEvaluator{};

    CHECK_FALSE(evaluator.matches(oldBinding, track));

    auto newContext = library::DictionaryReadContext{dictionary};
    auto const newBinding = PlanBinding{plan, newContext};
    CHECK(evaluator.matches(newBinding, track));
  }
} // namespace ao::query::test
