// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CliTestSupport.h"

#include <catch2/catch_test_macros.hpp>

namespace ao::cli::test
{
  TEST_CASE("CLI - init dry-run reports scan plan without importing tracks", "[cli][integration][init][dry-run]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"init", "--dry-run"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "new 1"));

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(result.out.empty());

    result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Test Title"));
  }
} // namespace ao::cli::test
