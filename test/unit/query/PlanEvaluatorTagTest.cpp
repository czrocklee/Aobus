// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/library/LibraryBinaryTestSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include "test/unit/query/PlanEvaluatorTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>
#include <ao/query/Field.h>
#include <ao/query/PlanEvaluator.h>
#include <ao/query/QueryCompiler.h>
#include <ao/query/detail/Bytecode.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <vector>

namespace ao::query::test
{
  TEST_CASE("PlanEvaluator - keeps OR candidates when only one branch uses tag bloom filtering",
            "[query][unit][plan-evaluator]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto dictionary = DictionaryStore{openDatabase(wtxn, "dictionary"), wtxn};
    auto aimerId = ao::test::requireValue(dictionary.put(wtxn, "Aimer"));
    REQUIRE(wtxn.commit());

    auto expr = parseOk(R"($artist ~ "Aimer" or #Aimer)");
    auto compiler = QueryCompiler{&dictionary};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    CHECK(plan.tagBloomMask == 0);

    auto artistMatchHotData = makeHotOnlyTrack(aimerId);
    auto artistMatchTrack = library::TrackView{artistMatchHotData, std::span<std::byte const>{}};
    CHECK(evaluator.matches(plan, artistMatchTrack) == true);

    auto tagIds = std::array<DictionaryId, 1>{aimerId};
    auto tagMatchHotData =
      makeHotOnlyTrack(kInvalidDictionaryId, kInvalidDictionaryId, kInvalidDictionaryId, kInvalidDictionaryId, tagIds);
    auto tagMatchTrack = library::TrackView{tagMatchHotData, std::span<std::byte const>{}};
    CHECK(evaluator.matches(plan, tagMatchTrack) == true);

    auto noMatchHotData = makeHotOnlyTrack();
    auto noMatchTrack = library::TrackView{noMatchHotData, std::span<std::byte const>{}};
    CHECK(evaluator.matches(plan, noMatchTrack) == false);
  }

  TEST_CASE("PlanEvaluator - rejects tag queries when tracks have no tags", "[query][unit][plan-evaluator]")
  {
    auto expr = parseOk("#rock");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {}};
    auto result = evaluator.matches(plan, track1.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - matches tag queries when the tag is present", "[query][unit][plan-evaluator]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto dictionary = DictionaryStore{openDatabase(wtxn, "dictionary"), wtxn};
    CHECK(dictionary.put(wtxn, "rock"));
    REQUIRE(wtxn.commit());

    auto expr = parseOk("#rock");
    auto compiler = QueryCompiler{&dictionary};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto trackWithTag =
      TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {0}};
    auto result = evaluator.matches(plan, trackWithTag.view());
    CHECK(result == true);
  }

  TEST_CASE("PlanEvaluator - matches numeric tag names and quoted custom keys", "[query][unit][plan-evaluator]")
  {
    auto spec = TrackSpec{};
    spec.tags.emplace_back("123");
    spec.customPairs.emplace_back("Replay Gain", "high");
    auto track = TestTrack{spec};

    auto const expression = parseOk(R"(#123 and %"Replay Gain" = "high")");
    auto const plan = compileOk(QueryCompiler{&track.dictionary()}, expression);

    CHECK(PlanEvaluator{}.evaluateFull(plan, track.view()));
  }

  TEST_CASE("PlanEvaluator - rejects tag queries when the tag is absent", "[query][unit][plan-evaluator]")
  {
    auto expr = parseOk("#rock");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto trackWithTag =
      TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {20}};
    auto result = evaluator.matches(plan, trackWithTag.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - compiles tag fields into field loads", "[query][unit][plan-evaluator]")
  {
    auto expr = parseOk("#tagname");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK(!plan.instructions.empty());
    CHECK(plan.instructions[0].op == OpCode::LoadField);
  }

  TEST_CASE("PlanEvaluator - leaves tag bloom mask empty without a dictionary", "[query][unit][plan-evaluator]")
  {
    auto expr = parseOk("#mytag");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK(plan.tagBloomMask == 0);
  }

  TEST_CASE("PlanEvaluator - leaves tag bloom mask empty when dictionary lookup misses",
            "[query][unit][plan-evaluator]")
  {
    auto expr = parseOk("#jazz");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK(plan.tagBloomMask == 0);
  }

  TEST_CASE("PlanEvaluator - leaves tag bloom mask empty without interned compiler dictionary ids",
            "[query][unit][plan-evaluator]")
  {
    auto expr = parseOk("#rock");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK(plan.tagBloomMask == 0);
  }

  TEST_CASE("PlanEvaluator - leaves multi-tag bloom mask empty without interned compiler dictionary ids",
            "[query][unit][plan-evaluator]")
  {
    auto expr = parseOk("#rock && #jazz");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK(plan.tagBloomMask == 0);
  }

  TEST_CASE("PlanEvaluator - reads track tag bloom bits from hot data", "[query][unit][plan-evaluator]")
  {
    {
      auto h = library::TrackHotHeader{};
      h.tagBloom = (1U << (10 & 31));
      auto data = serializeHeader(h);
      data.push_back(static_cast<std::byte>('\0'));
      auto view = library::TrackView{data, std::span<std::byte const>{}};
      CHECK(view.tags().bloom() == (1U << 10));
    }

    {
      auto h = library::TrackHotHeader{};
      h.tagBloom = (1U << (32 & 31));
      auto data = serializeHeader(h);
      data.push_back(static_cast<std::byte>('\0'));
      auto view = library::TrackView{data, std::span<std::byte const>{}};
      CHECK(view.tags().bloom() == 1U);
    }

    {
      auto h = library::TrackHotHeader{};
      h.tagBloom = (1U << (5 & 31)) | (1U << (20 & 31));
      auto data = serializeHeader(h);
      data.push_back(static_cast<std::byte>('\0'));
      auto view = library::TrackView{data, std::span<std::byte const>{}};
      CHECK((view.tags().bloom() & (1U << 5)) != 0);
      CHECK((view.tags().bloom() & (1U << 20)) != 0);
    }
  }

  TEST_CASE("PlanEvaluator - rejects tag bloom fast-path misses", "[query][unit][plan-evaluator]")
  {
    auto expr = parseOk("#mytag");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto h = library::TrackHotHeader{};
    h.tagBloom = 0x00000001U;

    auto data = std::vector<std::byte>{};
    data.insert_range(data.end(), utility::bytes::view(h));

    data.push_back(static_cast<std::byte>('\0'));
    data.push_back(static_cast<std::byte>('\0'));

    auto view = library::TrackView{data, std::span<std::byte const>{}};

    auto result = evaluator.matches(plan, view);
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - still verifies tag membership after bloom fast-path hits", "[query][unit][plan-evaluator]")
  {
    auto expr = parseOk("#mytag");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto h = library::TrackHotHeader{};
    h.tagBloom = 0xFFFFFFFFU;

    auto data = std::vector<std::byte>{};
    data.insert_range(data.end(), utility::bytes::view(h));

    data.push_back(static_cast<std::byte>('\0'));
    data.push_back(static_cast<std::byte>('\0'));

    auto view = library::TrackView{data, std::span<std::byte const>{}};

    auto result = evaluator.matches(plan, view);
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - verifies multi-tag bloom matches and collision candidates",
            "[query][unit][plan-evaluator]")
  {
    auto const spec = TrackSpec{.tags = {"rock", "jazz", "blues"}};
    auto track = TrackFixture{spec};
    auto const evaluator = PlanEvaluator{};
    auto compiler = QueryCompiler{&track.dictionary()};

    SECTION("Multi-Tag AND Requires All Bits")
    {
      auto const plan = compileOk(compiler, parseOk("#rock and #jazz"));
      CHECK(plan.tagBloomMask != 0);

      CHECK(evaluator.matches(plan, track.view()) == true);

      auto const spec2 = TrackSpec{.tags = {"rock"}};
      auto track2 = TrackFixture{spec2, &track.dictionary()};
      CHECK(evaluator.matches(plan, track2.view()) == false);
    }

    SECTION("Bloom Filter Collision - False Positive Mitigation")
    {
      auto& dictionary = track.dictionary();

      auto const* tagA = "rock";
      auto idA = dictionary.getOrIntern(tagA).raw();
      auto bitIndex = idA % 32;

      auto tagB = std::string{};

      for (std::int32_t i = 0; i < 1000; ++i)
      {
        auto const candidate = std::format("collision_tag_{}", i);

        if (auto const idB = dictionary.getOrIntern(candidate).raw(); idB != idA && (idB % 32) == bitIndex)
        {
          tagB = candidate;
          break;
        }
      }

      REQUIRE(!tagB.empty());

      auto const spec = TrackSpec{.tags = {tagA}};
      auto trackA = TrackFixture{spec, &dictionary};

      auto planB = compileOk(compiler, parseOk("#" + tagB));

      CHECK(evaluator.matches(planB, trackA.view()) == false);
    }
  }
} // namespace ao::query::test
