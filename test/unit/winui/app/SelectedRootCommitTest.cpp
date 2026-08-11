// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/app/SelectedRootCommit.h>

#include "test/unit/TestFixtureSupport.h"
#include <ao/utility/Path.h>
#include <ao/winui/DesktopSettingsYamlSchema.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

namespace ao::winui::test
{
  TEST_CASE("SelectedRootCommit - prepares a new settings candidate without dirtying live state", "[winui][unit][app]")
  {
    auto const fixture = ao::test::TempDir{};
    auto settings = DesktopSettings{};
    settings.lastLibraryPath = "C:/previous-library";

    auto candidateRes = prepareSelectedRootCommit(settings, fixture.path() / "new-library");

    REQUIRE(candidateRes);
    CHECK(candidateRes->lastLibraryPath == utility::pathToUtf8(fixture.path() / "new-library"));
    CHECK(settings.lastLibraryPath == "C:/previous-library");
  }
} // namespace ao::winui::test
