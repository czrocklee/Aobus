// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/FilesystemTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/utility/Path.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    std::filesystem::path copyFixtureAudio(MusicLibraryFixture const& libraryFixture, std::filesystem::path const& name)
    {
      auto const source = std::filesystem::path{AUDIO_TEST_DATA_DIR} / "empty.flac";

      INFO("Required audio fixture: " << source);
      REQUIRE(std::filesystem::is_regular_file(source));

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

  TEST_CASE("LibraryCommands - createTrackFromFile imports a valid file and publishes a mutation",
            "[runtime][unit][library][track-create]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& commands = commandsFixture.commands();

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onChanged([&](LibraryChangeSet const& event) noexcept { mutated = event.tracksMutated; });
    auto inserted = std::vector<TrackId>{};
    auto collectionSub = changes.onChanged([&](LibraryChangeSet const& ev) noexcept { inserted = ev.tracksInserted; });

    auto const absValidFile = copyFixtureAudio(libraryFixture, "music/song.flac");
    REQUIRE(std::filesystem::is_regular_file(absValidFile));

    auto const trackIdRes = commandsFixture.runTask(commands.createTrackFromFileAsync(absValidFile));
    REQUIRE(trackIdRes);
    CHECK(mutated.empty());
    REQUIRE(inserted.size() == 1);
    CHECK(inserted[0] == trackIdRes->trackId);

    auto transaction = libraryFixture.library().readTransaction();
    auto const optTrackView = libraryFixture.library()
                                .tracks()
                                .reader(transaction)
                                .get(trackIdRes->trackId, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optTrackView);
    CHECK(optTrackView->property().uri() == "music/song.flac");
    CHECK(libraryFixture.library().manifest().reader(transaction).get("music/song.flac"));

    auto const duplicateRes = commandsFixture.runTask(commands.createTrackFromFileAsync(absValidFile));
    REQUIRE(!duplicateRes);
    CHECK(duplicateRes.error().code == Error::Code::Conflict);
    CHECK(duplicateRes.error().message.contains("already imported"));
  }

  TEST_CASE("LibraryCommands - createTrackFromFile accepts root-relative paths",
            "[runtime][unit][library][track-create]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& commands = commandsFixture.commands();

    auto const absValidFile = copyFixtureAudio(libraryFixture, "relative.flac");
    REQUIRE(std::filesystem::is_regular_file(absValidFile));

    auto const trackIdRes = commandsFixture.runTask(commands.createTrackFromFileAsync("relative.flac"));
    REQUIRE(trackIdRes);

    auto transaction = libraryFixture.library().readTransaction();
    auto const optTrackView = libraryFixture.library()
                                .tracks()
                                .reader(transaction)
                                .get(trackIdRes->trackId, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optTrackView);
    CHECK(optTrackView->property().uri() == "relative.flac");
  }

  TEST_CASE("LibraryCommands - createTrackFromFile preserves a UTF-8 URI", "[runtime][unit][library][track-create]")
  {
    auto const expected = std::string{"\xE8\xAA\xB0\xE3\x81\x8B\xE3\x80\x81\xE6\xB5\xB7\xE3\x82\x92\xE3\x80\x82/"
                                      "Dvo\xC5\x99\xC3\xA1k.flac"};
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& commands = commandsFixture.commands();
    auto const mediaPath = copyFixtureAudio(libraryFixture, utility::pathFromUtf8(expected));

    REQUIRE(std::filesystem::is_regular_file(mediaPath));

    auto const createdRes = commandsFixture.runTask(commands.createTrackFromFileAsync(mediaPath));

    REQUIRE(createdRes);
    auto transaction = libraryFixture.library().readTransaction();
    auto const optTrack = libraryFixture.library()
                            .tracks()
                            .reader(transaction)
                            .get(createdRes->trackId, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optTrack);
    CHECK(optTrack->property().uri() == expected);
    CHECK(libraryFixture.library().manifest().reader(transaction).get(expected));
  }

  TEST_CASE("LibraryCommands - createTrackFromFile rejects unsupported files with Result errors",
            "[runtime][unit][library][track-create]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const existingId = libraryFixture.addTrack("Existing");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& commands = commandsFixture.commands();
    auto const revisionBefore = commandsFixture.bind(std::array{existingId}).revision();

    std::size_t publicationCount = 0;
    auto sub = changes.onChanged([&publicationCount](LibraryChangeSet const&) noexcept { ++publicationCount; });

    auto const unsupportedFile = createTextFile(libraryFixture, "unsupported.txt");
    REQUIRE(std::filesystem::is_regular_file(unsupportedFile));

    auto const trackIdRes = commandsFixture.runTask(commands.createTrackFromFileAsync(unsupportedFile));
    REQUIRE_FALSE(trackIdRes);
    CHECK(trackIdRes.error().code == Error::Code::NotSupported);
    CHECK(publicationCount == 0);

    auto transaction = libraryFixture.library().readTransaction();
    CHECK(libraryFixture.library().libraryRevision(transaction) == revisionBefore);
    auto const reader = libraryFixture.library().tracks().reader(transaction);
    CHECK(reader.entryCount() == 1);
    auto const optExisting = reader.get(existingId);
    REQUIRE(optExisting);
    CHECK(optExisting->metadata().title() == "Existing");
    CHECK_FALSE(libraryFixture.library().manifest().reader(transaction).get("unsupported.txt"));
  }

  TEST_CASE("LibraryCommands - createTrackFromFile rejects invalid path boundaries",
            "[runtime][unit][library][track-create]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& commands = commandsFixture.commands();

    SECTION("missing file")
    {
      auto const trackIdRes = commandsFixture.runTask(commands.createTrackFromFileAsync("missing.flac"));
      REQUIRE(!trackIdRes);
      CHECK(trackIdRes.error().code == Error::Code::NotFound);
    }

    SECTION("outside root")
    {
      auto const outsideTemp = ao::test::TempDir{};
      auto const outsideFile = outsideTemp.path() / "outside.flac";
      {
        auto out = std::ofstream{outsideFile};
        out << "not audio";
      }
      auto const trackIdRes = commandsFixture.runTask(commands.createTrackFromFileAsync(outsideFile));
      REQUIRE(!trackIdRes);
      CHECK(trackIdRes.error().code == Error::Code::InvalidInput);
      CHECK(trackIdRes.error().message.contains("outside music root"));
    }

    SECTION("symlink escaping root")
    {
      auto const outsideTemp = ao::test::TempDir{};
      auto const outsideFile = outsideTemp.path() / "outside.flac";
      {
        auto output = std::ofstream{outsideFile};
        output << "outside";
      }
      auto const alias = libraryFixture.root() / "alias.flac";
      auto const symlink = ao::test::SymlinkFixture{outsideFile, alias, ao::test::SymlinkType::File};

      auto const trackIdRes = commandsFixture.runTask(commands.createTrackFromFileAsync(alias));
      REQUIRE_FALSE(trackIdRes);
      CHECK(trackIdRes.error().code == Error::Code::InvalidInput);
      CHECK(trackIdRes.error().message.contains("outside music root"));
    }
  }
} // namespace ao::rt::test
