// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/rt/library/LibraryPaths.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace ao::rt::test
{
  TEST_CASE("LibraryPaths - derives the canonical paths for a music root", "[runtime][unit][library]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const paths = LibraryPaths{tempDir.path()};

    CHECK(paths.managedDataPath() == tempDir.path() / ".aobus");
    CHECK(paths.databasePath() == tempDir.path() / ".aobus" / "library");
    CHECK(paths.logsPath() == tempDir.path() / ".aobus" / "logs");
  }

  TEST_CASE("LibraryPaths - detects a database created by MusicLibrary", "[runtime][integration][library]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const paths = LibraryPaths{tempDir.path()};

    CHECK_FALSE(paths.hasExistingDatabase());

    std::filesystem::create_directories(paths.databasePath());
    CHECK_FALSE(paths.hasExistingDatabase());

    {
      auto const library = library::test::makeTestMusicLibrary(tempDir.path(), paths.databasePath());
      CHECK(library.databasePath() == paths.databasePath());
      CHECK(paths.hasExistingDatabase());
    }

    CHECK(paths.hasExistingDatabase());
  }
} // namespace ao::rt::test
