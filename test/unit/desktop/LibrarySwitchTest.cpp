// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/desktop/LibrarySwitch.h>

#include "test/unit/TestFixtureSupport.h"
#include <ao/Error.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace ao::desktop::test
{
  TEST_CASE("LibrarySwitch - active directory is reused after lexical normalization",
            "[runtime][unit][desktop-lifecycle]")
  {
    auto const fixture = ao::test::TempDir{};
    auto const active = fixture.path() / "music";
    REQUIRE(std::filesystem::create_directory(active));

    auto planRes = planLibrarySwitch(active, active / ".." / "music", true);

    REQUIRE(planRes);
    CHECK(planRes->disposition == LibrarySwitchDisposition::ReuseActive);
    CHECK(planRes->request.libraryRoot == std::filesystem::absolute(active).lexically_normal());
    CHECK(planRes->request.scanAfterOpen);
  }

#ifndef _WIN32

  TEST_CASE("LibrarySwitch - symlink alias reuses the active filesystem directory",
            "[runtime][regression][desktop-lifecycle]")
  {
    auto const fixture = ao::test::TempDir{};
    auto const active = fixture.path() / "music";
    auto const alias = fixture.path() / "music-alias";
    REQUIRE(std::filesystem::create_directory(active));
    std::filesystem::create_directory_symlink(active, alias);

    auto planRes = planLibrarySwitch(active, alias, false);

    REQUIRE(planRes);
    CHECK(planRes->disposition == LibrarySwitchDisposition::ReuseActive);
  }
#endif

  TEST_CASE("LibrarySwitch - different directory produces one normalized restart request",
            "[runtime][unit][desktop-lifecycle]")
  {
    auto const fixture = ao::test::TempDir{};
    auto const active = fixture.path() / "active";
    auto const requested = fixture.path() / "requested";
    REQUIRE(std::filesystem::create_directory(active));
    REQUIRE(std::filesystem::create_directory(requested));

    auto planRes = planLibrarySwitch(active, requested, false);

    REQUIRE(planRes);
    CHECK(planRes->disposition == LibrarySwitchDisposition::Restart);
    CHECK(planRes->request.libraryRoot == std::filesystem::absolute(requested).lexically_normal());
    CHECK_FALSE(planRes->request.scanAfterOpen);
  }

  TEST_CASE("LibrarySwitch - unavailable request fails before destructive retirement",
            "[runtime][unit][desktop-lifecycle]")
  {
    auto const fixture = ao::test::TempDir{};
    auto const active = fixture.path() / "active";
    REQUIRE(std::filesystem::create_directory(active));

    auto planRes = planLibrarySwitch(active, fixture.path() / "missing", false);

    REQUIRE_FALSE(planRes);
    CHECK(planRes.error().code == Error::Code::NotFound);
  }

  TEST_CASE("LibrarySwitch - unavailable active root does not block a different valid request",
            "[runtime][regression][desktop-lifecycle]")
  {
    auto const fixture = ao::test::TempDir{};
    auto const active = fixture.path() / "removed-active";
    auto const requested = fixture.path() / "requested";
    REQUIRE(std::filesystem::create_directory(requested));

    auto planRes = planLibrarySwitch(active, requested, false);

    REQUIRE(planRes);
    CHECK(planRes->disposition == LibrarySwitchDisposition::Restart);
    CHECK(planRes->request.libraryRoot == std::filesystem::absolute(requested).lexically_normal());
  }
} // namespace ao::desktop::test
