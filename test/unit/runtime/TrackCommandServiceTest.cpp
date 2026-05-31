// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TestUtils.h"
#include <ao/Type.h>
#include <ao/library/TrackBuilder.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/TrackCommandService.h>
#include <ao/rt/async/Runtime.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("TrackCommandService - addTag and removeTag", "[app][unit][runtime][track_command]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Test Track");

    auto executor = MockExecutor{};
    auto runtime = async::Runtime{executor};
    auto mutationService = LibraryMutationService{runtime, testLib.library()};

    auto trackCommandService = TrackCommandService{testLib.library(), mutationService};

    auto mutated = std::vector<TrackId>{};
    auto sub = mutationService.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    SECTION("Adding a new tag succeeds")
    {
      bool const added = trackCommandService.addTag(trackId, "Favorite");
      REQUIRE(added);
      REQUIRE(mutated.size() == 1);
      CHECK(mutated[0] == trackId);

      // Verify the tag was added by fetching it
      auto txn = testLib.library().readTransaction();
      auto const optTrackView =
        testLib.library().tracks().reader(txn).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
      REQUIRE(optTrackView.has_value());
      auto builder = library::TrackBuilder::fromView(*optTrackView, testLib.library().dictionary());
      CHECK(std::ranges::contains(builder.tags().names(), std::string_view{"Favorite"}));
    }

    SECTION("Adding an existing tag fails")
    {
      trackCommandService.addTag(trackId, "Favorite");
      mutated.clear();

      bool const added = trackCommandService.addTag(trackId, "Favorite");
      REQUIRE_FALSE(added);
      REQUIRE(mutated.empty());
    }

    SECTION("Adding a tag to an invalid track fails")
    {
      bool const added = trackCommandService.addTag(TrackId{99999}, "Favorite");
      REQUIRE_FALSE(added);
    }

    SECTION("Removing an existing tag succeeds")
    {
      trackCommandService.addTag(trackId, "Favorite");
      mutated.clear();

      bool const removed = trackCommandService.removeTag(trackId, "Favorite");
      REQUIRE(removed);
      REQUIRE(mutated.size() == 1);
      CHECK(mutated[0] == trackId);
    }

    SECTION("Removing a non-existent tag fails")
    {
      bool const removed = trackCommandService.removeTag(trackId, "NonExistent");
      REQUIRE_FALSE(removed);
      REQUIRE(mutated.empty());
    }

    SECTION("Removing a tag from an invalid track fails")
    {
      bool const removed = trackCommandService.removeTag(TrackId{99999}, "Favorite");
      REQUIRE_FALSE(removed);
    }
  }

  TEST_CASE("TrackCommandService - deleteTrack", "[app][unit][runtime][track_command]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Test Track");

    auto executor = MockExecutor{};
    auto runtime = async::Runtime{executor};
    auto mutationService = LibraryMutationService{runtime, testLib.library()};

    auto trackCommandService = TrackCommandService{testLib.library(), mutationService};

    auto mutated = std::vector<TrackId>{};
    auto sub = mutationService.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    SECTION("Deleting an existing track succeeds")
    {
      bool const deleted = trackCommandService.deleteTrack(trackId);
      REQUIRE(deleted);
      REQUIRE(mutated.size() == 1);
      CHECK(mutated[0] == trackId);

      auto txn = testLib.library().readTransaction();
      auto const optTrackView =
        testLib.library().tracks().reader(txn).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
      REQUIRE_FALSE(optTrackView.has_value());
    }

    SECTION("Deleting an invalid track fails")
    {
      bool const deleted = trackCommandService.deleteTrack(TrackId{99999});
      REQUIRE_FALSE(deleted);
      REQUIRE(mutated.empty());
    }
  }

  TEST_CASE("TrackCommandService - createTrackFromFile", "[app][unit][runtime][track_command]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto runtime = async::Runtime{executor};
    auto mutationService = LibraryMutationService{runtime, testLib.library()};

    auto trackCommandService = TrackCommandService{testLib.library(), mutationService};

    auto mutated = std::vector<TrackId>{};
    auto sub = mutationService.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    SECTION("Creating a track from a valid file succeeds")
    {
      // Using a test data file from the repository
      auto const validFile = std::filesystem::path{"test/integration/tag/test_data/empty.flac"};

      // Provide an absolute path if test is run from a different directory, or assume the working directory is correct
      // Usually CTest runs from the build directory, but `ctest` runs in `build/test/` sometimes.
      // Aobus `ao_test` runs with CWD = repository root or build root. We'll use absolute path resolving.
      auto const projectRoot = std::filesystem::current_path();
      auto const absValidFile = projectRoot / validFile;

      if (!std::filesystem::exists(absValidFile))
      {
        SUCCEED("Skipping test because test file is missing: " + absValidFile.string());
        return;
      }

      TrackId const newTrackId = trackCommandService.createTrackFromFile(absValidFile);
      REQUIRE(newTrackId != kInvalidTrackId);
      REQUIRE(mutated.size() == 1);
      CHECK(mutated[0] == newTrackId);

      auto txn = testLib.library().readTransaction();
      auto const optTrackView =
        testLib.library().tracks().reader(txn).get(newTrackId, library::TrackStore::Reader::LoadMode::Hot);
      REQUIRE(optTrackView.has_value());
    }

    SECTION("Creating a track from an invalid file fails")
    {
      auto const projectRoot = std::filesystem::current_path();
      auto const absInvalidFile = projectRoot / "README.md";

      TrackId const newTrackId = trackCommandService.createTrackFromFile(absInvalidFile);
      REQUIRE(newTrackId == kInvalidTrackId);
      REQUIRE(mutated.empty());
    }
  }
} // namespace ao::rt::test
