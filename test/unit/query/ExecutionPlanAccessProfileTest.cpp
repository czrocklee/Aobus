// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/library/TrackViewTestSupport.h"
#include "test/unit/query/ExecutionPlanTestSupport.h"
#include <ao/library/TrackView.h>
#include <ao/query/Field.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string>

namespace ao::query::test
{
  TEST_CASE("ExecutionPlan - access profiles validate independent TrackView tiers", "[query][unit][execution-plan]")
  {
    auto const hotData = library::test::makeMinimalHotTrackViewData();
    auto const coldData = library::test::makeColdTrackViewData();
    auto const hotView = library::TrackView{hotData, std::span<std::byte const>{}};
    auto const coldView = library::TrackView{std::span<std::byte const>{}, coldData};
    auto const bothView = library::TrackView{hotData, coldData};
    auto const malformedData = std::array<std::byte, 1>{};
    auto const malformedView = library::TrackView{malformedData, malformedData};

    CHECK(hasRequiredTrackData(AccessProfile::NoTrackData, hotView));
    CHECK(hasRequiredTrackData(AccessProfile::HotOnly, hotView));
    CHECK_FALSE(hasRequiredTrackData(AccessProfile::ColdOnly, hotView));
    CHECK_FALSE(hasRequiredTrackData(AccessProfile::HotAndCold, hotView));

    CHECK_FALSE(hasRequiredTrackData(AccessProfile::HotOnly, coldView));
    CHECK(hasRequiredTrackData(AccessProfile::ColdOnly, coldView));
    CHECK_FALSE(hasRequiredTrackData(AccessProfile::HotAndCold, coldView));

    CHECK(hasRequiredTrackData(AccessProfile::HotAndCold, bothView));
    CHECK(hasRequiredTrackData(AccessProfile::NoTrackData, malformedView));
    CHECK_FALSE(hasRequiredTrackData(AccessProfile::HotOnly, malformedView));
    CHECK_FALSE(hasRequiredTrackData(AccessProfile::ColdOnly, malformedView));
  }

  TEST_CASE("ExecutionPlan - reports HotOnly access for hot metadata", "[query][unit][execution-plan]")
  {
    // Metadata variable -> HotOnly
    auto expr = parseOk("$artist = Bach");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK(plan.accessProfile == AccessProfile::HotOnly);
  }

  TEST_CASE("ExecutionPlan - reports ColdOnly access for custom metadata", "[query][unit][execution-plan]")
  {
    // Custom variable -> ColdOnly
    auto expr = parseOk("%customkey = value");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - reports HotAndCold access for mixed predicates", "[query][unit][execution-plan]")
  {
    // Mix of hot and cold -> HotAndCold
    auto expr = parseOk("$artist = Bach && %customkey = value");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK(plan.accessProfile == AccessProfile::HotAndCold);
  }

  TEST_CASE("ExecutionPlan - reports ColdOnly access for cold property fields", "[query][unit][execution-plan]")
  {
    // Property variable -> ColdOnly (stored in TrackColdHeader)
    auto expr = parseOk("@duration > 180000");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - reports HotOnly access for tag fields", "[query][unit][execution-plan]")
  {
    // Tag variable -> HotOnly
    auto expr = parseOk("#rock");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK(plan.accessProfile == AccessProfile::HotOnly);
  }

  TEST_CASE("ExecutionPlan - reports ColdOnly access for cold metadata fields", "[query][unit][execution-plan]")
  {
    // TrackNumber field is in cold storage -> ColdOnly
    auto expr = parseOk("$trackNumber > 5");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - reports ColdOnly access for classical role metadata", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("$conductor = Kleiber");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - reports ColdOnly access for duration", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("@duration > 180000");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - reports ColdOnly access for bitrate", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("@bitrate > 320");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - reports HotOnly access for sample rate", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("@sampleRate = 44100");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    CHECK(plan.accessProfile == AccessProfile::HotOnly);
  }

  TEST_CASE("ExecutionPlan - reports ColdOnly access for channels", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("@channels = 2");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - reports HotAndCold access for hot and cold fields", "[query][unit][execution-plan]")
  {
    // Mix of hot ($year) and cold ($trackNumber) -> HotAndCold
    auto expr = parseOk("$year > 2020 && $trackNumber > 5");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK(plan.accessProfile == AccessProfile::HotAndCold);
  }

  TEST_CASE("ExecutionPlan - reports ColdOnly access for custom fields", "[query][unit][execution-plan]")
  {
    // Custom variable -> ColdOnly
    auto expr = parseOk("%customkey = value");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - classifies access profiles exhaustively", "[query][unit][execution-plan]")
  {
    auto compiler = QueryCompiler{};

    SECTION("HotOnly")
    {
      auto fields = {"$title",
                     "$artist",
                     "$album",
                     "$genre",
                     "$albumArtist",
                     "$composer",
                     "$year",
                     "#rock",
                     "@sampleRate",
                     "@bitDepth",
                     "@codec"};

      for (auto const* f : fields)
      {
        auto expr = parseOk(f);

        if (f[0] != '#' && std::string{f} != "true" && std::string{f} != "false")
        {
          expr = parseOk(std::string{f} + " = 0");
        }

        auto plan = compileOk(compiler, expr);
        CHECK(plan.accessProfile == AccessProfile::HotOnly);
      }
    }

    SECTION("NoTrackData")
    {
      for (auto const* f : {"true", "false"})
      {
        auto plan = compileOk(compiler, parseOk(f));
        CHECK(plan.accessProfile == AccessProfile::NoTrackData);
      }
    }

    SECTION("ColdOnly")
    {
      auto fields = {"$trackNumber",
                     "$trackTotal",
                     "$discNumber",
                     "$discTotal",
                     "$coverArt",
                     "%isrc",
                     "@duration",
                     "@bitrate",
                     "@channels"};

      for (auto const* f : fields)
      {
        auto expr = parseOk(std::string{f} + " >= 0");
        auto plan = compileOk(compiler, expr);
        CHECK(plan.accessProfile == AccessProfile::ColdOnly);
      }

      // $work is a dictionary field (cold), so reference it with equality rather
      // than an ordered comparison, which is rejected for dictionary fields.
      auto workPlan = compileOk(compiler, parseOk("$work = w"));
      CHECK(workPlan.accessProfile == AccessProfile::ColdOnly);
    }

    SECTION("HotAndCold")
    {
      auto expr = parseOk("$year >= 2020 and @duration >= 3m");
      auto plan = compileOk(compiler, expr);
      CHECK(plan.accessProfile == AccessProfile::HotAndCold);
    }
  }
} // namespace ao::query::test
