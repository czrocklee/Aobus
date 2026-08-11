// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/desktop/LibraryStartupPlanner.h>

#include "test/unit/TestFixtureSupport.h"
#include <ao/Error.h>
#include <ao/desktop/LibrarySwitch.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace ao::desktop::test
{
  TEST_CASE("LibraryStartupPlanner - explicit successor is strict and defers root durability",
            "[runtime][unit][desktop-lifecycle]")
  {
    auto const fixture = ao::test::TempDir{};
    auto const root = fixture.path() / "successor";
    REQUIRE(std::filesystem::create_directory(root));

    auto planRes = planLibraryStartup({
      .optSuccessorRequest = LibrarySwitchRequest{.libraryRoot = root, .scanAfterOpen = true},
      .optPersistedRoot = fixture.path() / "persisted",
      .emptyLibraryRoot = fixture.path() / "fallback",
    });

    REQUIRE(planRes);
    CHECK(planRes->libraryRoot == std::filesystem::absolute(root).lexically_normal());
    CHECK(planRes->source == LibraryStartupRootSource::ExplicitSuccessor);
    CHECK(planRes->playbackPersistence == PlaybackPersistenceStartup::AwaitDurableRoot);
    CHECK(planRes->optSelectedRootCommit == planRes->libraryRoot);
    CHECK(planRes->scanAfterOpen);
  }

  TEST_CASE("LibraryStartupPlanner - missing explicit root fails instead of selecting fallback",
            "[runtime][unit][desktop-lifecycle]")
  {
    auto const fixture = ao::test::TempDir{};
    auto planRes = planLibraryStartup({
      .optSuccessorRequest = LibrarySwitchRequest{.libraryRoot = fixture.path() / "missing"},
      .emptyLibraryRoot = fixture.path() / "fallback",
    });

    REQUIRE_FALSE(planRes);
    CHECK(planRes.error().code == Error::Code::NotFound);
  }

  TEST_CASE("LibraryStartupPlanner - valid persisted root restores without a pending commit",
            "[runtime][unit][desktop-lifecycle]")
  {
    auto const fixture = ao::test::TempDir{};
    auto const root = fixture.path() / "persisted";
    REQUIRE(std::filesystem::create_directory(root));

    auto planRes = planLibraryStartup({
      .optPersistedRoot = root,
      .emptyLibraryRoot = fixture.path() / "fallback",
    });

    REQUIRE(planRes);
    CHECK(planRes->libraryRoot == std::filesystem::absolute(root).lexically_normal());
    CHECK(planRes->source == LibraryStartupRootSource::Persisted);
    CHECK(planRes->playbackPersistence == PlaybackPersistenceStartup::Restore);
    CHECK_FALSE(planRes->optSelectedRootCommit);
  }

  TEST_CASE("LibraryStartupPlanner - invalid persisted root selects fallback without creating it",
            "[runtime][unit][desktop-lifecycle]")
  {
    auto const fixture = ao::test::TempDir{};
    auto const fallback = fixture.path() / "fallback";
    auto planRes = planLibraryStartup({
      .optPersistedRoot = fixture.path() / "missing",
      .emptyLibraryRoot = fallback,
    });

    REQUIRE(planRes);
    CHECK(planRes->libraryRoot == std::filesystem::absolute(fallback).lexically_normal());
    CHECK(planRes->source == LibraryStartupRootSource::EmptyLibraryFallback);
    CHECK(planRes->playbackPersistence == PlaybackPersistenceStartup::Restore);
    CHECK_FALSE(std::filesystem::exists(fallback));
  }
} // namespace ao::desktop::test
