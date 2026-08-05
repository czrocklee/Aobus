// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/app/LibraryStartupPlan.h>

#include "test/unit/TestFixtureSupport.h"
#include <ao/utility/Path.h>
#include <ao/winui/DesktopSettingsYamlSchema.h>
#include <ao/winui/app/StartupOptions.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

namespace ao::winui::test
{
  TEST_CASE("LibraryStartupPlan - explicit root is validated and committed only after activation", "[winui][unit][app]")
  {
    auto const fixture = ao::test::TempDir{};
    auto const musicRoot = fixture.path() / "My Music";
    REQUIRE(std::filesystem::create_directories(musicRoot));

    auto settings = DesktopSettings{};
    settings.lastLibraryPath = "C:/previous-library";
    auto const originalSettings = settings;
    auto const options = StartupOptions{.optLibraryRoot = musicRoot};

    auto planRes = planLibraryStartup(options, settings, fixture.path() / "empty-library");

    REQUIRE(planRes);
    CHECK(planRes->source == LibraryStartupRootSource::Explicit);
    CHECK(planRes->libraryRoot == std::filesystem::absolute(musicRoot).lexically_normal());
    REQUIRE(planRes->optSelectedRootCommit);
    CHECK(planRes->optSelectedRootCommit->root == planRes->libraryRoot);
    CHECK(settings == originalSettings);

    commitSelectedRoot(*planRes, settings);

    CHECK(settings.lastLibraryPath == utility::pathToUtf8(planRes->libraryRoot));
  }

  TEST_CASE("LibraryStartupPlan - explicit missing root fails instead of using the fallback", "[winui][unit][app]")
  {
    auto const fixture = ao::test::TempDir{};
    auto settings = DesktopSettings{};
    settings.lastLibraryPath = "C:/previous-library";
    auto const originalSettings = settings;
    auto const options = StartupOptions{.optLibraryRoot = fixture.path() / "missing-library"};

    auto planRes = planLibraryStartup(options, settings, fixture.path() / "empty-library");

    REQUIRE_FALSE(planRes);
    CHECK(planRes.error().code == Error::Code::NotFound);
    CHECK(settings == originalSettings);
  }

  TEST_CASE("LibraryStartupPlan - valid persisted root is reused without a pending commit", "[winui][unit][app]")
  {
    auto const fixture = ao::test::TempDir{};
    auto const musicRoot = fixture.path() / "Persisted Music";
    REQUIRE(std::filesystem::create_directories(musicRoot));

    auto settings = DesktopSettings{};
    settings.lastLibraryPath = utility::pathToUtf8(musicRoot);
    auto const originalSettings = settings;

    auto planRes = planLibraryStartup(StartupOptions{}, settings, fixture.path() / "empty-library");

    REQUIRE(planRes);
    CHECK(planRes->source == LibraryStartupRootSource::Persisted);
    CHECK(planRes->libraryRoot == std::filesystem::absolute(musicRoot).lexically_normal());
    CHECK_FALSE(planRes->optSelectedRootCommit);
    CHECK(settings == originalSettings);
  }

  TEST_CASE("LibraryStartupPlan - invalid persisted root uses fallback without changing settings", "[winui][unit][app]")
  {
    auto const fixture = ao::test::TempDir{};
    auto settings = DesktopSettings{};
    settings.lastLibraryPath = utility::pathToUtf8(fixture.path() / "missing-library");
    auto const originalSettings = settings;
    auto const fallback = fixture.path() / "empty-library";

    auto planRes = planLibraryStartup(StartupOptions{}, settings, fallback);

    REQUIRE(planRes);
    CHECK(planRes->source == LibraryStartupRootSource::EmptyLibraryFallback);
    CHECK(planRes->libraryRoot == std::filesystem::absolute(fallback).lexically_normal());
    CHECK_FALSE(planRes->optSelectedRootCommit);
    CHECK(settings == originalSettings);
  }
} // namespace ao::winui::test
