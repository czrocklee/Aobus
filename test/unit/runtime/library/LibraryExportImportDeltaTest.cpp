// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/FilesystemTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryJobs.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/LibraryYamlImporter.h>
#include <ao/utility/Uuid.h>
#include <ao/yaml/RymlAdapter.h>

#include <c4/yml/tree.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::library;
  namespace yaml = ao::yaml;

  namespace
  {
    ryml::Tree loadTree(std::filesystem::path const& path, std::vector<char>& buffer)
    {
      auto bufferRes = yaml::readFileResult(path);
      REQUIRE(bufferRes);
      buffer = std::move(*bufferRes);
      auto state = yaml::ErrorCallbackState{path.string()};
      auto tree = ryml::Tree{yaml::callbacks()};
      REQUIRE(yaml::parseInPlace(tree, buffer, state));
      return tree;
    }

    Result<ImportReport> importThroughRuntime(library::MusicLibrary& library,
                                              LibraryChanges& changes,
                                              QueuedExecutor& executor,
                                              std::filesystem::path const& path,
                                              ImportMode mode)
    {
      auto runtime = async::Runtime{executor};
      auto runtimeLibraryPtr = ao::test::requireValue(Library::create(runtime, library, changes));
      auto planRes = runQueuedTask(runtime, executor, runtimeLibraryPtr->jobs().prepareLibraryImportAsync(path, mode));

      if (!planRes)
      {
        return std::unexpected{planRes.error()};
      }

      return runQueuedTask(
        runtime, executor, runtimeLibraryPtr->jobs().applyLibraryImportPlanAsync(std::move(*planRes)));
    }
  } // namespace

  TEST_CASE("LibraryYaml - delta export writes changed and unreadable tracks",
            "[runtime][workflow][import-export][delta]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());

    auto coverResourceId = kInvalidResourceId;
    {
      auto transaction = library::test::writeTransaction(ml);
      auto result = library::test::physicalWriter(ml.resources(), transaction)
                      .create(std::vector{std::byte{1}, std::byte{2}, std::byte{3}});
      REQUIRE(result);
      coverResourceId = *result;
      REQUIRE(transaction.commit());
    }
    library::test::addTrackWithUniqueFixtureUri(ml,
                                                library::test::TrackSpec{.title = "Should Export Fully",
                                                                         .artist = "",
                                                                         .album = "",
                                                                         .uri = "no-file.flac",
                                                                         .year = 0,
                                                                         .discNumber = 0,
                                                                         .trackNumber = 0,
                                                                         .duration = std::chrono::milliseconds{0},
                                                                         .bitrate = Bitrate{},
                                                                         .sampleRate = SampleRate{},
                                                                         .channels = Channels{},
                                                                         .bitDepth = BitDepth{}});
    library::test::addTrackWithUniqueFixtureUri(
      ml,
      library::test::TrackSpec{.title = "Will fallback to full export because media file read fails",
                               .artist = "",
                               .album = "",
                               .uri = "dummy.flac",
                               .year = 0,
                               .discNumber = 0,
                               .trackNumber = 0,
                               .duration = std::chrono::milliseconds{0},
                               .bitrate = Bitrate{},
                               .sampleRate = SampleRate{},
                               .channels = Channels{},
                               .bitDepth = BitDepth{}});
    library::test::addTrackWithUniqueFixtureUri(ml,
                                                library::test::TrackSpec{.title = "Different Title",
                                                                         .artist = "",
                                                                         .album = "",
                                                                         .uri = "cover.flac",
                                                                         .coverArtId = coverResourceId,
                                                                         .year = 0,
                                                                         .discNumber = 0,
                                                                         .trackNumber = 0,
                                                                         .duration = std::chrono::milliseconds{0},
                                                                         .bitrate = Bitrate{},
                                                                         .sampleRate = SampleRate{},
                                                                         .channels = Channels{},
                                                                         .bitDepth = BitDepth{}});
    library::test::addTrackWithUniqueFixtureUri(ml,
                                                library::test::TrackSpec{.title = "",
                                                                         .artist = "",
                                                                         .album = "",
                                                                         .uri = "cover-removed.flac",
                                                                         .year = 0,
                                                                         .discNumber = 0,
                                                                         .trackNumber = 0,
                                                                         .duration = std::chrono::milliseconds{0},
                                                                         .bitrate = Bitrate{},
                                                                         .sampleRate = SampleRate{},
                                                                         .channels = Channels{},
                                                                         .bitDepth = BitDepth{}});

    std::filesystem::copy_file(std::filesystem::path{AUDIO_TEST_DATA_DIR} / "with_cover.flac",
                               std::filesystem::path{temp.path()} / "cover.flac");
    std::filesystem::copy_file(std::filesystem::path{AUDIO_TEST_DATA_DIR} / "with_cover.flac",
                               std::filesystem::path{temp.path()} / "cover-removed.flac");

    auto const yamlPath = std::filesystem::path{temp.path()} / "delta.yaml";
    auto exporter = LibraryYamlExporter{ml};
    REQUIRE(exporter.exportToYaml(yamlPath, rt::ExportMode::Delta));

    {
      auto buffer = std::vector<char>{};
      auto tree = loadTree(yamlPath, buffer);
      auto root = tree.rootref();
      auto tracks = root["library"]["tracks"];
      REQUIRE(tracks.is_seq());
      REQUIRE(tracks.num_children() == 4);

      CHECK(yaml::scalarView(tracks[0]["title"]) == "Should Export Fully");
      CHECK(yaml::scalarView(tracks[1]["title"]) == "Will fallback to full export because media file read fails");
      CHECK(yaml::scalarView(tracks[2]["title"]) == "Different Title");

      // A delta document carries no embedded cover in version 5. The library holds
      // what the last scan saw, so a file retagged since then makes the two
      // differ, and the sequence delta would carry is the stale one: applying it
      // would overwrite the covers the baseline just read from the file with
      // references to content that may exist nowhere.
      CHECK_FALSE(tracks[2].has_child("covers"));
      CHECK_FALSE(tracks[3].has_child("covers"));
      CHECK_FALSE(root["library"].has_child("resources"));
      CHECK(yaml::scalarView(tracks[1]["title"]) == "Will fallback to full export because media file read fails");
    }
  }

  TEST_CASE("LibraryYaml - delta export reports filesystem inspection errors",
            "[runtime][workflow][import-export][delta]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    auto const blockedDir = std::filesystem::path{temp.path()} / "blocked";
    std::filesystem::create_directory(blockedDir);
    auto const blockedFile = blockedDir / "song.flac";
    std::ofstream{blockedFile} << "content";

    {
      library::test::addTrackWithUniqueFixtureUri(ml,
                                                  library::test::TrackSpec{.title = "Cannot inspect baseline",
                                                                           .artist = "",
                                                                           .album = "",
                                                                           .uri = "blocked/song.flac",
                                                                           .year = 0,
                                                                           .discNumber = 0,
                                                                           .trackNumber = 0,
                                                                           .duration = std::chrono::milliseconds{0},
                                                                           .bitrate = Bitrate{},
                                                                           .sampleRate = SampleRate{},
                                                                           .channels = Channels{},
                                                                           .bitDepth = BitDepth{}});
    }

    auto const denied = ao::test::ScopedDirectoryAccessGuard{blockedDir, ao::test::DeniedDirectoryAccess::Read};

    if (!denied.effective())
    {
      SKIP("the current process bypasses directory read restrictions");
    }

    auto exporter = LibraryYamlExporter{ml};
    auto const result = exporter.exportToYaml(std::filesystem::path{temp.path()} / "delta.yaml", ExportMode::Delta);

    REQUIRE(!result);
    CHECK(result.error().code == Error::Code::IoError);
  }

  TEST_CASE("LibraryYaml - delta import reports filesystem inspection errors",
            "[runtime][workflow][import-export][delta]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    auto importer = LibraryYamlImporter{ml};
    auto const yamlPath = std::filesystem::path{temp.path()} / "delta-import.yaml";
    auto const blockedDir = std::filesystem::path{temp.path()} / "blocked";
    std::filesystem::create_directory(blockedDir);
    auto const blockedFile = blockedDir / "song.flac";
    std::ofstream{blockedFile} << "content";

    {
      auto yaml = std::ofstream{yamlPath};
      yaml << "version: 5\n"
           << "export_mode: delta\n"
           << "library:\n"
           << "  tracks:\n"
           << "    - uri: \"blocked/song.flac\"\n"
           << "      title: Cannot inspect baseline\n"
           << "  lists: []\n";
    }

    auto const denied = ao::test::ScopedDirectoryAccessGuard{blockedDir, ao::test::DeniedDirectoryAccess::Read};

    if (!denied.effective())
    {
      SKIP("the current process bypasses directory read restrictions");
    }

    auto const result = importer.importFromYamlOffline(yamlPath);

    REQUIRE(!result);
    CHECK(result.error().code == Error::Code::IoError);
  }

  TEST_CASE("LibraryYaml - merge publishes truthful inserted and mutated track ids",
            "[runtime][workflow][import-export][changeset]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    auto const existingId = library::test::addTrackWithUniqueFixtureUri(
      ml, library::test::TrackSpec{.title = "Before", .artist = "", .album = "", .uri = "existing.flac"});

    auto const yamlPath = std::filesystem::path{temp.path()} / "changes.yaml";
    {
      auto yaml = std::ofstream{yamlPath};
      yaml << R"(version: 5
export_mode: delta
library:
  tracks:
    - uri: existing.flac
      title: After
    - uri: inserted.flac
      title: Inserted
  lists: []
)";
    }

    auto executor = QueuedExecutor{};
    auto changes = makeLibraryChanges(executor, ml);
    auto observed = std::vector<LibraryChangeSet>{};
    auto subscription =
      changes.onChanged([&observed](LibraryChangeSet const& value) noexcept { observed.push_back(value); });
    REQUIRE(importThroughRuntime(ml, changes, executor, yamlPath, ImportMode::Merge));

    REQUIRE(observed.size() == 1);
    REQUIRE(observed.front().tracksInserted.size() == 1);
    CHECK(observed.front().tracksInserted.front() != existingId);
    CHECK(observed.front().tracksMutated == std::vector{existingId});
    CHECK_FALSE(observed.front().libraryReset);
    auto transaction = ml.readTransaction();
    CHECK(observed.front().libraryRevision == ml.libraryRevision(transaction));
  }

  TEST_CASE("LibraryYaml - restore publishes a library reset", "[runtime][workflow][import-export][changeset]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    auto const yamlPath = std::filesystem::path{temp.path()} / "restore.yaml";
    {
      auto yaml = std::ofstream{yamlPath};
      yaml << "version: 5\nexport_mode: full\nlibrary:\n  resources: []\n  tracks: []\n  lists: []\n";
    }

    auto executor = QueuedExecutor{};
    auto changes = makeLibraryChanges(executor, ml);
    auto observed = std::vector<LibraryChangeSet>{};
    auto subscription =
      changes.onChanged([&observed](LibraryChangeSet const& value) noexcept { observed.push_back(value); });
    REQUIRE(importThroughRuntime(ml, changes, executor, yamlPath, ImportMode::Restore));

    REQUIRE(observed.size() == 1);
    CHECK(observed.front().libraryReset);
    auto transaction = ml.readTransaction();
    CHECK(observed.front().libraryRevision == ml.libraryRevision(transaction));
  }

  TEST_CASE("LibraryYaml - restore commits library id and content under one revision",
            "[runtime][workflow][import-export][changeset]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    auto const yamlPath = std::filesystem::path{temp.path()} / "restore-with-id.yaml";
    {
      auto yaml = std::ofstream{yamlPath};
      yaml << "version: 5\n"
           << "libraryId: 123E4567-E89B-12D3-A456-426614174000\n"
           << "export_mode: full\n"
           << "library:\n"
           << "  resources: []\n"
           << "  tracks: []\n"
           << "  lists: []\n";
    }

    auto executor = QueuedExecutor{};
    auto changes = makeLibraryChanges(executor, ml);
    auto observed = std::vector<LibraryChangeSet>{};
    auto subscription =
      changes.onChanged([&observed](LibraryChangeSet const& value) noexcept { observed.push_back(value); });
    REQUIRE(importThroughRuntime(ml, changes, executor, yamlPath, ImportMode::Restore));

    CHECK(utility::formatUuid(ml.metadataHeader().libraryId) == "123e4567-e89b-12d3-a456-426614174000");
    REQUIRE(observed.size() == 1);
    CHECK(observed.front().libraryReset);
    auto transaction = ml.readTransaction();
    CHECK(observed.front().libraryRevision == ml.libraryRevision(transaction));
  }

  TEST_CASE("LibraryYaml - preview preserves library id and publishes no changes",
            "[runtime][workflow][import-export][dry-run]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    auto const originalLibraryId = ml.metadataHeader().libraryId;
    auto const yamlPath = std::filesystem::path{temp.path()} / "preview-with-id.yaml";
    {
      auto yaml = std::ofstream{yamlPath};
      yaml << "version: 5\n"
           << "libraryId: 123e4567-e89b-12d3-a456-426614174000\n"
           << "export_mode: full\n"
           << "library:\n"
           << "  resources: []\n"
           << "  tracks: []\n"
           << "  lists: []\n";
    }

    auto revisionTransaction = ml.readTransaction();
    auto const originalRevision = ml.libraryRevision(revisionTransaction);
    auto importer = LibraryYamlImporter{ml};

    REQUIRE(importer.previewImportFromYamlOffline(yamlPath, ImportMode::Restore));

    CHECK(ml.metadataHeader().libraryId == originalLibraryId);
    auto afterTransaction = ml.readTransaction();
    CHECK(ml.libraryRevision(afterTransaction) == originalRevision);
  }
} // namespace ao::rt::test
