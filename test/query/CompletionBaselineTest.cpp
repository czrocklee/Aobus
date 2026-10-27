// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/query/Completion.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <print>
#include <string_view>
#include <variant>
#include <vector>

namespace ao::query::test
{
  // This representative loop is a diagnostic workload, not portable performance evidence.
  // Its inputs are checked independently before timing so semantic regressions cannot hide in the aggregate.
  TEST_CASE("Completion - exercises representative analyzer workload", "[query][unit][completion][baseline]")
  {
    auto const inputs = std::array<std::string_view, 4>{
      R"($artist = "Miles Davis" )",
      R"($artist = "Miles Davis" an)",
      "$ar",
      R"($album = "Kind" and $ye)",
    };

    auto const optEmptyLogicalAnalysis = analyzeQueryCompletion(inputs[0], inputs[0].size());
    REQUIRE(optEmptyLogicalAnalysis);
    auto const* emptyLogical = std::get_if<QueryLogicalOperatorCompletion>(&*optEmptyLogicalAnalysis);
    REQUIRE(emptyLogical != nullptr);
    CHECK(emptyLogical->replacement.replaceBegin == 23);
    CHECK(emptyLogical->replacement.replaceEnd == 24);
    CHECK(emptyLogical->replacement.prefix.empty());
    CHECK(completeQueryLogicalOperator(emptyLogical->replacement.prefix) ==
          std::vector<std::string_view>{"and", "or", "&&", "||"});

    auto const optPartialLogicalAnalysis = analyzeQueryCompletion(inputs[1], inputs[1].size());
    REQUIRE(optPartialLogicalAnalysis);
    auto const* partialLogical = std::get_if<QueryLogicalOperatorCompletion>(&*optPartialLogicalAnalysis);
    REQUIRE(partialLogical != nullptr);
    CHECK(partialLogical->replacement.replaceBegin == 23);
    CHECK(partialLogical->replacement.replaceEnd == 26);
    CHECK(partialLogical->replacement.prefix == "an");
    CHECK(completeQueryLogicalOperator(partialLogical->replacement.prefix) == std::vector<std::string_view>{"and"});

    auto const optArtistAnalysis = analyzeQueryCompletion(inputs[2], inputs[2].size());
    REQUIRE(optArtistAnalysis);
    auto const* artist = std::get_if<QueryCompletionToken>(&*optArtistAnalysis);
    REQUIRE(artist != nullptr);
    CHECK(artist->type == VariableType::Metadata);
    CHECK(artist->replaceBegin == 0);
    CHECK(artist->replaceEnd == 3);
    CHECK(artist->prefix == "ar");
    auto const artistMatches = completeQueryVariable(artist->type, artist->prefix);
    REQUIRE(artistMatches.size() == 1);
    CHECK(artistMatches[0].type == VariableType::Metadata);
    CHECK(artistMatches[0].field == Field::ArtistId);
    CHECK(artistMatches[0].canonicalName == "artist");
    CHECK(artistMatches[0].kind == QueryVariableCompletionMatchKind::CanonicalPrefix);

    auto const optYearAnalysis = analyzeQueryCompletion(inputs[3], inputs[3].size());
    REQUIRE(optYearAnalysis);
    auto const* year = std::get_if<QueryCompletionToken>(&*optYearAnalysis);
    REQUIRE(year != nullptr);
    CHECK(year->type == VariableType::Metadata);
    CHECK(year->replaceBegin == 20);
    CHECK(year->replaceEnd == 23);
    CHECK(year->prefix == "ye");
    auto const yearMatches = completeQueryVariable(year->type, year->prefix);
    REQUIRE(yearMatches.size() == 1);
    CHECK(yearMatches[0].type == VariableType::Metadata);
    CHECK(yearMatches[0].field == Field::Year);
    CHECK(yearMatches[0].canonicalName == "year");
    CHECK(yearMatches[0].kind == QueryVariableCompletionMatchKind::CanonicalPrefix);

    constexpr std::int32_t kIterations = 20000;
    std::size_t sink = 0;

    auto const start = std::chrono::steady_clock::now();

    for (std::int32_t i = 0; i < kIterations; ++i)
    {
      for (auto const input : inputs)
      {
        if (analyzeQueryCompletion(input, input.size()))
        {
          ++sink;
        }
      }
    }

    auto const elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
    auto const calls = static_cast<std::int64_t>(kIterations) * static_cast<std::int64_t>(inputs.size());

    std::println("=== Completion diagnostic workload: {} calls, {} ns/call ===", calls, elapsed.count() / calls);
    CHECK(sink == static_cast<std::size_t>(calls));
  }
} // namespace ao::query::test
