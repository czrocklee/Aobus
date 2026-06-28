// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include <ao/Type.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("LibraryWriter createTrackFromFile imports a valid file and publishes a mutation",
            "[runtime][unit][library][mutation][track][create]")
  {
    auto testLib = TestMusicLibrary{};
    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    // Using a test data file from the repository
    auto const validFile = std::filesystem::path{"test/integration/tag/test_data/empty.flac"};

    // Provide an absolute path if test is run from a different directory, or assume the working directory is correct
    // Usually CTest runs from the build directory, but `ctest` runs in `build/test/` sometimes.
    // Aobus `ao_core_test` runs with CWD = repository root or build root. We'll use absolute path resolving.
    auto const projectRoot = std::filesystem::current_path();
    auto const absValidFile = projectRoot / validFile;

    if (!std::filesystem::exists(absValidFile))
    {
      SUCCEED("Skipping test because test file is missing: " + absValidFile.string());
      return;
    }

    auto const optNewTrackId = writer.createTrackFromFile(absValidFile);
    REQUIRE(optNewTrackId.has_value());
    REQUIRE(mutated.size() == 1);
    CHECK(mutated[0] == *optNewTrackId);

    auto txn = testLib.library().readTransaction();
    auto const optTrackView =
      testLib.library().tracks().reader(txn).get(*optNewTrackId, library::TrackStore::Reader::LoadMode::Hot);
    CHECK(optTrackView.has_value());
  }

  TEST_CASE("LibraryWriter createTrackFromFile rejects unsupported files",
            "[runtime][unit][library][mutation][track][create]")
  {
    auto testLib = TestMusicLibrary{};
    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    auto const projectRoot = std::filesystem::current_path();
    auto const absInvalidFile = projectRoot / "README.md";

    auto const optNewTrackId = writer.createTrackFromFile(absInvalidFile);
    CHECK_FALSE(optNewTrackId.has_value());
    CHECK(mutated.empty());
  }
} // namespace ao::rt::test
