// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/FilesystemTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibraryJobs.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/utility/Path.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <utility>

namespace ao::rt::test
{
  namespace
  {
    // The CJK character and musical note occupy seven UTF-8 bytes but three UTF-16 code units.
    constexpr std::string_view kWavName = "\xE9\x9F\xB3\xF0\x9F\x8E\xB5.wav";

    std::filesystem::path extendedFixturePath(std::filesystem::path path)
    {
      auto const& native = path.make_preferred().native();

      if (native.starts_with(L"\\\\"))
      {
        return std::filesystem::path{L"\\\\?\\UNC\\" + native.substr(2)};
      }

      return std::filesystem::path{L"\\\\?\\" + native};
    }

    std::string paddedWavUri(std::size_t padding)
    {
      constexpr std::size_t kDirectoryLength = 100;
      REQUIRE(padding >= 2);
      auto uri = std::string{};

      // Pad with directories, not a single filename. This also exercises descent past MAX_PATH.
      while (padding > kDirectoryLength + 2)
      {
        uri.append(kDirectoryLength, 'a');
        uri += '/';
        padding -= kDirectoryLength + 1;
      }

      uri.append(padding - 1, 'b');
      uri += '/';
      uri += kWavName;
      return uri;
    }

    std::string wavUriAtNativeLength(std::filesystem::path const& root, std::size_t const length)
    {
      auto const baseLength = (root / utility::pathFromUtf8(kWavName)).native().size();
      REQUIRE(length >= baseLength + 2);
      auto uri = paddedWavUri(length - baseLength);
      REQUIRE((root / utility::pathFromUtf8(uri)).native().size() == length);
      return uri;
    }

    void installWav(std::filesystem::path const& absolutePath)
    {
      // Fixture I/O must not depend on machine long-path policy or Aobus path resolution.
      auto const target = extendedFixturePath(absolutePath);
      auto const source = audio::test::requireAudioFixture("basic_metadata.wav");
      std::filesystem::create_directories(target.parent_path());
      REQUIRE(std::filesystem::copy_file(source, target));
      REQUIRE(std::filesystem::file_size(target) == std::filesystem::file_size(source));
    }

    void renameFixtureCase(std::filesystem::path const& original, std::filesystem::path const& renamed)
    {
      // An intermediate name prevents a case-only rename from becoming a no-op.
      auto const temporary = original.parent_path() / "case-change.tmp";
      std::filesystem::rename(original, temporary);
      std::filesystem::rename(temporary, renamed);
    }

    void checkEmptyLibrary(library::MusicLibrary const& storage)
    {
      auto transaction = storage.readTransaction();
      CHECK(storage.libraryRevision(transaction) == 0);
      CHECK(storage.tracks().reader(transaction).entryCount() == 0);
      auto const manifest = storage.manifest().reader(transaction);
      CHECK(manifest.begin() == manifest.end());
    }

    void checkStoredWav(library::MusicLibrary const& storage, TrackId id, std::string_view uri)
    {
      auto transaction = storage.readTransaction();
      auto const reader = storage.tracks().reader(transaction);
      CHECK(reader.entryCount() == 1);
      auto const optTrack = reader.get(id, library::TrackStore::Reader::LoadMode::Both);
      REQUIRE(optTrack);
      CHECK(optTrack->property().uri() == uri);
      CHECK(optTrack->metadata().title() == "Test Title");
      auto const optManifest = storage.manifest().reader(transaction).get(uri);
      REQUIRE(optManifest);
      CHECK(optManifest->trackId() == id);
    }
  } // namespace

  TEST_CASE("LibraryCommands - imports Windows media at and beyond MAX_PATH")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto& storage = libraryFixture.library();
    auto changes = makeStateOnlyLibraryChanges(storage);
    auto commandsFixture = LibraryCommandsFixture{storage, changes};
    auto& commands = commandsFixture.commands();
    std::size_t length = 0;
    bool useExtendedInput = false;

    SECTION("259 UTF-16 code units")
    {
      length = 259;
    }

    SECTION("260 UTF-16 code units")
    {
      length = 260;
    }

    SECTION("explicit extended input at 320 UTF-16 code units")
    {
      length = 320;
      useExtendedInput = true;
    }

    auto const uri = wavUriAtNativeLength(libraryFixture.root(), length);
    auto const relativePath = utility::pathFromUtf8(uri);
    auto const absolutePath = libraryFixture.root() / relativePath;
    installWav(absolutePath);

    auto const previewRes = commandsFixture.runTask(commands.previewCreateTrackFromFileAsync(relativePath));
    INFO((previewRes ? "preview succeeded" : previewRes.error().message));
    REQUIRE(previewRes);
    CHECK(previewRes->uri == uri);
    CHECK(previewRes->title == "Test Title");
    checkEmptyLibrary(storage);

    auto const input = useExtendedInput ? extendedFixturePath(absolutePath) : absolutePath;
    auto const createdRes = commandsFixture.runTask(commands.createTrackFromFileAsync(input));
    INFO((createdRes ? "import succeeded" : createdRes.error().message));
    REQUIRE(createdRes);
    CHECK(createdRes->uri == uri);
    checkStoredWav(storage, createdRes->trackId, uri);
  }

  TEST_CASE("LibraryCommands - rejects missing and over-limit Windows media paths without writing")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto& storage = libraryFixture.library();
    auto changes = makeStateOnlyLibraryChanges(storage);
    auto commandsFixture = LibraryCommandsFixture{storage, changes};
    auto& commands = commandsFixture.commands();

    SECTION("missing file in an existing long directory")
    {
      auto const uri = wavUriAtNativeLength(libraryFixture.root(), 320);
      auto const absolutePath = libraryFixture.root() / utility::pathFromUtf8(uri);
      auto const target = extendedFixturePath(absolutePath);
      REQUIRE(std::filesystem::create_directories(target.parent_path()));
      REQUIRE_FALSE(std::filesystem::exists(target));

      auto const createdRes = commandsFixture.runTask(commands.createTrackFromFileAsync(absolutePath));
      REQUIRE_FALSE(createdRes);
      CHECK(createdRes.error().code == Error::Code::NotFound);
    }

    SECTION("existing file one byte beyond the URI limit")
    {
      constexpr std::size_t kOverLimitLength = 1977;
      auto const uri = paddedWavUri(kOverLimitLength - kWavName.size());
      REQUIRE(uri.size() == kOverLimitLength);
      auto const absolutePath = libraryFixture.root() / utility::pathFromUtf8(uri);
      installWav(absolutePath);

      auto const createdRes = commandsFixture.runTask(commands.createTrackFromFileAsync(absolutePath));
      REQUIRE_FALSE(createdRes);
      CHECK(createdRes.error().code == Error::Code::ValueTooLarge);
    }

    checkEmptyLibrary(storage);
  }

  TEST_CASE("LibraryScan - imports and rediscovers long Windows media after reopening the library")
  {
    auto const temp = ao::test::TempDir{};
    auto const root = temp.path() / "music";
    auto const databasePath = temp.path() / "db";
    auto const uri = wavUriAtNativeLength(root, 320);
    installWav(root / utility::pathFromUtf8(uri));

    auto trackId = kInvalidTrackId;

    {
      auto storage = library::test::makeTestMusicLibrary(root, databasePath);
      auto changes = makeStateOnlyLibraryChanges(storage);
      auto commandsFixture = LibraryCommandsFixture{storage, changes};
      auto& jobs = commandsFixture.library().jobs();
      auto planRes = commandsFixture.runTask(jobs.buildScanPlanAsync());
      INFO((planRes ? "scan planned" : planRes.error().message));
      REQUIRE(planRes);
      REQUIRE(planRes->size() == 1);
      auto const& item = planRes->items().front();
      INFO(item.errorMessage);
      REQUIRE(item.classification == ScanClassification::New);
      CHECK(item.uri == uri);

      auto const appliedRes = commandsFixture.runTask(jobs.applyScanPlanAsync(std::move(*planRes)));
      INFO((appliedRes ? "scan applied" : appliedRes.error().message));
      REQUIRE(appliedRes);
      CHECK(appliedRes->failureCount == 0);
      REQUIRE(appliedRes->insertedIds.size() == 1);
      trackId = appliedRes->insertedIds.front();
    }

    auto const reopened = library::test::makeTestMusicLibrary(root, databasePath);
    checkStoredWav(reopened, trackId, uri);
    auto const rescannedRes = LibraryScan{reopened}.buildPlan();
    INFO((rescannedRes ? "scan planned after reopening" : rescannedRes.error().message));
    REQUIRE(rescannedRes);
    REQUIRE(rescannedRes->size() == 1);
    auto const& item = rescannedRes->items().front();
    CHECK(item.classification == ScanClassification::Unchanged);
    CHECK(item.uri == uri);
    CHECK(item.trackId == trackId);
  }

  TEST_CASE("LibraryScan - rekeys long Windows CUE carriers after a case-only rename")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const& root = libraryFixture.root();
    auto const longUri = wavUriAtNativeLength(root, 320);
    auto const prefix = longUri.substr(0, longUri.rfind('/') + 1);
    auto const oldUri = prefix + "A.wav";
    auto const newUri = prefix + "a.wav";
    auto const original = root / utility::pathFromUtf8(oldUri);
    auto const originalIo = extendedFixturePath(original);
    auto const renamedIo = extendedFixturePath(root / utility::pathFromUtf8(newUri));
    REQUIRE(original.native().size() > 260);
    installWav(original);

    if (!std::filesystem::exists(renamedIo))
    {
      SKIP("The temporary Windows carrier directory is case-sensitive");
    }

    REQUIRE(std::filesystem::equivalent(originalIo, renamedIo));
    auto const originalSheet = originalIo.parent_path() / "A.cue";

    {
      auto sheet = std::ofstream{originalSheet, std::ios::binary};
      sheet << "FILE \"A.wav\" WAVE\nTRACK 01 AUDIO\nTITLE \"First CUE member\"\nINDEX 01 00:00:00\n"
               "TRACK 02 AUDIO\nTITLE \"Second CUE member\"\nINDEX 01 00:00:50\n";
      REQUIRE(sheet.good());
    }

    auto& storage = libraryFixture.library();
    auto changes = makeStateOnlyLibraryChanges(storage);
    auto commandsFixture = LibraryCommandsFixture{storage, changes};
    auto& commands = commandsFixture.commands();
    auto const createdRes = commandsFixture.runTask(commands.createTrackFromFileAsync(original));
    INFO((createdRes ? "long CUE carrier imported" : createdRes.error().message));
    REQUIRE(createdRes);
    REQUIRE(createdRes->trackIds.size() == 2);

    renameFixtureCase(originalIo, renamedIo);
    renameFixtureCase(originalSheet, originalSheet.parent_path() / "a.cue");

    auto& jobs = commandsFixture.library().jobs();
    auto planRes = commandsFixture.runTask(jobs.buildScanPlanAsync());
    INFO((planRes ? "renamed carrier scanned" : planRes.error().message));
    REQUIRE(planRes);
    REQUIRE(planRes->size() == 1);
    auto const& item = planRes->items().front();
    INFO(item.errorMessage);
    REQUIRE(item.classification == ScanClassification::SpellingChanged);
    CHECK(item.uri == newUri);
    CHECK(item.oldUri == oldUri);

    auto const appliedRes = commandsFixture.runTask(jobs.applyScanPlanAsync(std::move(*planRes)));
    INFO((appliedRes ? "renamed carrier reapplied" : appliedRes.error().message));
    REQUIRE(appliedRes);
    CHECK(appliedRes->failureCount == 0);
    CHECK(appliedRes->insertedIds.empty());
    CHECK(appliedRes->relinkedIds == createdRes->trackIds);

    auto transaction = storage.readTransaction();
    auto const manifest = storage.manifest().reader(transaction);
    CHECK_FALSE(manifest.get(oldUri));
    auto const optCarrier = manifest.get(newUri);
    REQUIRE(optCarrier);
    CHECK(std::ranges::equal(optCarrier->segmentTrackIds(), createdRes->trackIds));
    auto const tracks = storage.tracks().reader(transaction);
    auto const optSecond = tracks.get(createdRes->trackIds[1], library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optSecond);
    CHECK(optSecond->property().uri() == newUri);
    CHECK(optSecond->metadata().title() == "Second CUE member");
    REQUIRE(optSecond->property().segment());
    CHECK(optSecond->property().segment()->startFrame == 29400);
  }

  TEST_CASE("LibraryScan - rekeys tracks after a long Windows directory changes case")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const& root = libraryFixture.root();
    auto const parentUri = utility::pathFromUtf8(wavUriAtNativeLength(root, 320)).parent_path();
    auto const oldDirectory = parentUri / "Album";
    auto const newDirectory = parentUri / "album";
    auto const suffix = utility::pathFromUtf8("disc/track.wav");
    auto const oldUri = utility::pathToGenericUtf8(oldDirectory / suffix);
    auto const newUri = utility::pathToGenericUtf8(newDirectory / suffix);
    REQUIRE((root / parentUri).native().size() > 260);
    installWav(root / oldDirectory / suffix);
    auto const original = extendedFixturePath(root / oldDirectory);
    auto const renamed = extendedFixturePath(root / newDirectory);

    if (!std::filesystem::exists(renamed))
    {
      SKIP("The temporary Windows music directory is case-sensitive");
    }

    auto& storage = libraryFixture.library();
    auto changes = makeStateOnlyLibraryChanges(storage);
    auto commandsFixture = LibraryCommandsFixture{storage, changes};
    auto const createdRes =
      commandsFixture.runTask(commandsFixture.commands().createTrackFromFileAsync(root / oldDirectory / suffix));
    REQUIRE(createdRes);
    renameFixtureCase(original, renamed);

    auto& jobs = commandsFixture.library().jobs();
    auto planRes = commandsFixture.runTask(jobs.buildScanPlanAsync());
    REQUIRE(planRes);
    REQUIRE(planRes->size() == 1);
    auto const& item = planRes->items().front();
    INFO(item.errorMessage);
    REQUIRE(item.classification == ScanClassification::SpellingChanged);
    CHECK(item.oldUri == oldUri);
    CHECK(item.uri == newUri);
    CHECK(item.fullPath == root / newDirectory / suffix);
    auto const appliedRes = commandsFixture.runTask(jobs.applyScanPlanAsync(std::move(*planRes)));
    REQUIRE(appliedRes);
    CHECK(appliedRes->failureCount == 0);
    CHECK(appliedRes->relinkedIds == createdRes->trackIds);
    checkStoredWav(storage, createdRes->trackId, newUri);
    auto transaction = storage.readTransaction();
    CHECK_FALSE(storage.manifest().reader(transaction).get(oldUri));
  }

  TEST_CASE("LibraryScan - does not descend into a Windows junction to an ancestor")
  {
    auto const temp = ao::test::TempDir{};
    auto const root = temp.path() / "music";
    auto const uri = wavUriAtNativeLength(root, 320);
    auto const track = root / utility::pathFromUtf8(uri);
    auto const album = track.parent_path();
    installWav(track);
    auto const alias = album / "back";
    REQUIRE(alias.native().size() > 260);
    auto const aliasIo = extendedFixturePath(alias);
    auto const albumIo = extendedFixturePath(album);
    auto const storage = library::test::makeTestMusicLibrary(root, temp.path() / "db");

    {
      auto const junction = ao::test::JunctionFixture{std::filesystem::path{album.generic_wstring()}, alias};
      REQUIRE(std::filesystem::equivalent(aliasIo, albumIo));

      // If descent is accidentally enabled, fail at its first child rather than
      // walking the cycle until Windows reaches its path or reparse limit.
      bool visitedAlias = false;
      auto const planRes = LibraryScan{storage}.buildPlan(
        [&](std::filesystem::path const& path)
        {
          REQUIRE(path.parent_path() != alias);
          visitedAlias = visitedAlias || path == alias;
        });
      REQUIRE(planRes);
      CHECK(visitedAlias);
      REQUIRE(planRes->size() == 1);
      auto const& item = planRes->items().front();
      CHECK(item.classification == ScanClassification::New);
      CHECK(item.uri == uri);
    }

    CHECK_FALSE(std::filesystem::exists(aliasIo));
    CHECK(std::filesystem::is_regular_file(extendedFixturePath(track)));
  }
} // namespace ao::rt::test
