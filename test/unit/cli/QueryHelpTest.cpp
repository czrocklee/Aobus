// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CliTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"

#include <catch2/catch_test_macros.hpp>

namespace ao::cli::test
{
  TEST_CASE("CLI - query help and errors teach filter usage", "[cli][unit][contract]")
  {
    auto fixture = CliFixture{};
    fixture.addTrack(library::test::TrackSpec{});

    auto result = fixture.run({"track", "show", "--help"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "aobus track show 1 2 3"));
    CHECK(contains(result.out, "not $genre?"));
    CHECK(contains(result.out, "$artist + \" - \" + $title"));
    CHECK(contains(result.out, "$movement($m)"));
    CHECK(contains(result.out, "%customKey"));

    result = fixture.run({"track", "show", "--filter", "("});
    checkDomainFailure(result, "hint: expressions look like:");
    CHECK(contains(result.err, "$genre($g)"));
    CHECK(contains(result.err, "%customKey"));

    result = fixture.run({"track", "show", "--filter", "$gerne = Jazz"});
    checkDomainFailure(result, "did you mean '$genre'?");
    CHECK(contains(result.err, "available metadata fields:"));
  }
} // namespace ao::cli::test
