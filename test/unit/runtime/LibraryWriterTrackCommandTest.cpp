// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TestUtils.h"
#include <ao/Type.h>
#include <ao/library/TrackBuilder.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("LibraryWriter - editTags add and remove", "[app][unit][runtime][track_command]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Test Track");

    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    auto const favorite = std::array{std::string{"Favorite"}};

    SECTION("Adding a new tag succeeds")
    {
      auto const reply = writer.editTags(std::array{trackId}, favorite, {});
      REQUIRE_FALSE(reply.mutatedIds.empty());
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

    SECTION("Adding an existing tag is a no-op")
    {
      writer.editTags(std::array{trackId}, favorite, {});
      mutated.clear();

      auto const reply = writer.editTags(std::array{trackId}, favorite, {});
      CHECK(reply.mutatedIds.empty());
      CHECK(mutated.empty());
    }

    SECTION("Adding a tag to a non-existent track is a no-op")
    {
      auto const reply = writer.editTags(std::array{TrackId{99999}}, favorite, {});
      CHECK(reply.mutatedIds.empty());
    }

    SECTION("Removing an existing tag succeeds")
    {
      writer.editTags(std::array{trackId}, favorite, {});
      mutated.clear();

      auto const reply = writer.editTags(std::array{trackId}, {}, favorite);
      REQUIRE_FALSE(reply.mutatedIds.empty());
      REQUIRE(mutated.size() == 1);
      CHECK(mutated[0] == trackId);
    }

    SECTION("Removing a non-existent tag is a no-op")
    {
      auto const nonExistent = std::array{std::string{"NonExistent"}};
      auto const reply = writer.editTags(std::array{trackId}, {}, nonExistent);
      CHECK(reply.mutatedIds.empty());
      CHECK(mutated.empty());
    }

    SECTION("Removing a tag from a non-existent track is a no-op")
    {
      auto const reply = writer.editTags(std::array{TrackId{99999}}, {}, favorite);
      CHECK(reply.mutatedIds.empty());
    }
  }

  TEST_CASE("LibraryWriter - deleteTrack", "[app][unit][runtime][track_command]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Test Track");

    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    SECTION("Deleting an existing track succeeds")
    {
      bool const deleted = writer.deleteTrack(trackId);
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
      bool const deleted = writer.deleteTrack(TrackId{99999});
      REQUIRE_FALSE(deleted);
      REQUIRE(mutated.empty());
    }
  }

  TEST_CASE("LibraryWriter - createTrackFromFile", "[app][unit][runtime][track_command]")
  {
    auto testLib = TestMusicLibrary{};
    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    SECTION("Creating a track from a valid file succeeds")
    {
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
      REQUIRE(optTrackView.has_value());
    }

    SECTION("Creating a track from an invalid file fails")
    {
      auto const projectRoot = std::filesystem::current_path();
      auto const absInvalidFile = projectRoot / "README.md";

      auto const optNewTrackId = writer.createTrackFromFile(absInvalidFile);
      REQUIRE_FALSE(optNewTrackId.has_value());
      REQUIRE(mutated.empty());
    }
  }
} // namespace ao::rt::test
