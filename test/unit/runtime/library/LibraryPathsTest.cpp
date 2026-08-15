// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/LibraryPaths.h>

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <ios>

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

  TEST_CASE("LibraryPaths - an empty data file is not an existing database", "[runtime][unit][library]")
  {
    // The Windows data-file preparation creates the file before LMDB initializes
    // it, so a preparation or open that fails afterwards leaves an empty one. Read
    // as an existing library it would skip the first scan and leave the library
    // permanently empty, so only a file with content counts.
    auto const tempDir = ao::test::TempDir{};
    auto const paths = LibraryPaths{tempDir.path()};
    std::filesystem::create_directories(paths.databasePath());

    auto const dataPath = paths.databasePath() / "data.mdb";
    {
      std::ofstream{dataPath, std::ios::binary};
    }

    REQUIRE(std::filesystem::exists(dataPath));
    CHECK_FALSE(paths.hasExistingDatabase());
  }
} // namespace ao::rt::test
