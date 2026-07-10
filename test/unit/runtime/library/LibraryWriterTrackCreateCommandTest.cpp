// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    std::filesystem::path copyFixtureAudio(MusicLibraryFixture const& libraryFixture, std::string const& name)
    {
      auto const source = std::filesystem::path{TAG_TEST_DATA_DIR} / "empty.flac";

      if (!std::filesystem::exists(source))
      {
        return {};
      }

      auto const destination = libraryFixture.root() / name;
      std::filesystem::create_directories(destination.parent_path());
      std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
      return destination;
    }

    std::filesystem::path createTextFile(MusicLibraryFixture const& libraryFixture, std::string const& name)
    {
      auto const destination = libraryFixture.root() / name;
      auto out = std::ofstream{destination};
      out << "not audio";
      return destination;
    }
  } // namespace

  TEST_CASE("LibraryWriter - createTrackFromFile imports a valid file and publishes a mutation",
            "[runtime][unit][library][track-create]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{libraryFixture.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });
    auto inserted = std::vector<TrackId>{};
    auto collectionSub = changes.onTrackCollectionChanged([&](auto const& ev) { inserted = ev.inserted; });

    auto const absValidFile = copyFixtureAudio(libraryFixture, "music/song.flac");

    if (!std::filesystem::exists(absValidFile))
    {
      SUCCEED("Skipping test because test file is missing");
      return;
    }

    auto const trackIdResult = writer.createTrackFromFile(absValidFile);
    REQUIRE(trackIdResult);
    REQUIRE(mutated.size() == 1);
    CHECK(mutated[0] == trackIdResult->trackId);
    REQUIRE(inserted.size() == 1);
    CHECK(inserted[0] == trackIdResult->trackId);

    auto transaction = libraryFixture.library().readTransaction();
    auto const optTrackView = libraryFixture.library()
                                .tracks()
                                .reader(transaction)
                                .get(trackIdResult->trackId, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optTrackView);
    CHECK(optTrackView->property().uri() == "music/song.flac");
    CHECK(libraryFixture.library().manifest().reader(transaction).get("music/song.flac"));

    auto const duplicateResult = writer.createTrackFromFile(absValidFile);
    REQUIRE(!duplicateResult);
    CHECK(duplicateResult.error().code == Error::Code::Conflict);
    CHECK(duplicateResult.error().message.contains("already imported"));
  }

  TEST_CASE("LibraryWriter - createTrackFromFile accepts root-relative paths", "[runtime][unit][library][track-create]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{libraryFixture.library(), changes};

    auto const absValidFile = copyFixtureAudio(libraryFixture, "relative.flac");

    if (!std::filesystem::exists(absValidFile))
    {
      SUCCEED("Skipping test because test file is missing");
      return;
    }

    auto const trackIdResult = writer.createTrackFromFile("relative.flac");
    REQUIRE(trackIdResult);

    auto transaction = libraryFixture.library().readTransaction();
    auto const optTrackView = libraryFixture.library()
                                .tracks()
                                .reader(transaction)
                                .get(trackIdResult->trackId, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optTrackView);
    CHECK(optTrackView->property().uri() == "relative.flac");
  }

  TEST_CASE("LibraryWriter - createTrackFromFile rejects unsupported files with Result errors",
            "[runtime][unit][library][track-create]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{libraryFixture.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    auto const unsupportedFile = createTextFile(libraryFixture, "unsupported.txt");

    auto const trackIdResult = writer.createTrackFromFile(unsupportedFile);
    REQUIRE(!trackIdResult);
    CHECK(trackIdResult.error().code == Error::Code::NotSupported);
    CHECK(mutated.empty());
  }

  TEST_CASE("LibraryWriter - createTrackFromFile rejects invalid path boundaries",
            "[runtime][unit][library][track-create]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{libraryFixture.library(), changes};

    SECTION("missing file")
    {
      auto const trackIdResult = writer.createTrackFromFile("missing.flac");
      REQUIRE(!trackIdResult);
      CHECK(trackIdResult.error().code == Error::Code::NotFound);
    }

    SECTION("outside root")
    {
      auto const outsideTemp = ao::test::TempDir{};
      auto const outsideFile = outsideTemp.path() / "outside.flac";
      {
        auto out = std::ofstream{outsideFile};
        out << "not audio";
      }
      auto const trackIdResult = writer.createTrackFromFile(outsideFile);
      REQUIRE(!trackIdResult);
      CHECK(trackIdResult.error().code == Error::Code::InvalidInput);
      CHECK(trackIdResult.error().message.contains("outside music root"));
    }
  }
} // namespace ao::rt::test
