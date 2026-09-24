// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "runtime/library/LibraryYamlExporter.h"
#include "runtime/library/LibraryYamlImporter.h"
#include "test/unit/FilesystemTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/async/TaskFuture.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListStore.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryImportPlan.h>
#include <ao/rt/library/LibraryJobs.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/library/LibraryTransfer.h>
#include <ao/utility/Uuid.h>
#include <ao/yaml/RymlAdapter.h>

#include <c4/yml/tree.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::library;
  namespace yaml = ao::yaml;

  namespace
  {
    // Imports exercise disk-backed preparation and commit, not a latency contract.
    constexpr auto kImportTaskTimeout = std::chrono::seconds{10};

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

    Result<ImportReport> importThroughRuntime(CoreRuntime& core,
                                              QueuedExecutor& executor,
                                              std::filesystem::path const& path,
                                              ImportMode mode)
    {
      auto& runtime = core.async();
      auto& runtimeLibrary = core.library();
      INFO("Preparing library import");
      auto planRes = runQueuedTask(
        runtime, executor, runtimeLibrary.jobs().prepareLibraryImportAsync(path, mode), kImportTaskTimeout);

      if (!planRes)
      {
        return std::unexpected{planRes.error()};
      }

      INFO("Applying prepared library import");
      return runQueuedTask(
        runtime, executor, runtimeLibrary.jobs().applyLibraryImportPlanAsync(std::move(*planRes)), kImportTaskTimeout);
    }
  } // namespace

  TEST_CASE("LibraryYaml - failed import wait retires the suspended task during runtime unwinding",
            "[runtime][unit][import-export][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto optFuture = std::optional<async::TaskFuture<Result<LibraryImportPlan>>>{};
    auto const run = [&]
    {
      auto executorPtr = std::make_unique<QueuedExecutor>();
      auto& executor = *executorPtr;
      auto core = ao::test::requireValue(CoreRuntime::create(
        std::move(executorPtr), temp.path(), temp.path(), {}, library::test::kTestMusicLibraryMapBytes));
      optFuture.emplace(core.async().spawn(
        flagCompletionAsync(completedPtr,
                            core.library().jobs().prepareLibraryImportAsync(
                              std::filesystem::path{temp.path()} / "absent.yaml", ImportMode::Merge))));
      executor.checkQueued();
      CHECK_FALSE(completedPtr->load());
      // Model a failed test assertion while the real import is suspended at its first executor hop.
      throw std::runtime_error{"import wait failed"};
    };
    REQUIRE_THROWS_AS(run(), std::runtime_error);
    REQUIRE(completedPtr->load());
    REQUIRE(optFuture);
    CHECK_THROWS_AS(optFuture->get(), std::future_error);
    auto reopened = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    CHECK(Library::prepare(reopened));
  }

  TEST_CASE("LibraryYaml - delta export writes changed and unreadable tracks",
            "[runtime][integration][import-export][delta]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());

    auto coverResourceId = kInvalidResourceId;
    {
      auto transaction = library::test::writeTransaction(ml);
      auto res = library::test::physicalWriter(ml.resources(), transaction)
                   .create(std::vector{std::byte{1}, std::byte{2}, std::byte{3}});
      REQUIRE(res);
      coverResourceId = *res;
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

      CHECK(yaml::scalarView(tracks[0]["uri"]) == "no-file.flac");
      CHECK(yaml::scalarView(tracks[1]["uri"]) == "dummy.flac");
      CHECK(yaml::scalarView(tracks[2]["uri"]) == "cover.flac");
      CHECK(yaml::scalarView(tracks[3]["uri"]) == "cover-removed.flac");
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
    }
  }

  TEST_CASE("LibraryYaml - delta export reports filesystem inspection errors", "[runtime][unit][import-export][delta]")
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

    if (!denied.isEffective())
    {
      SKIP("the current process bypasses directory read restrictions");
    }

    auto exporter = LibraryYamlExporter{ml};
    auto const res = exporter.exportToYaml(std::filesystem::path{temp.path()} / "delta.yaml", ExportMode::Delta);

    REQUIRE(!res);
    CHECK(res.error().code == Error::Code::IoError);
  }

  TEST_CASE("LibraryYaml - delta import reports filesystem inspection errors", "[runtime][unit][import-export][delta]")
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

    if (!denied.isEffective())
    {
      SKIP("the current process bypasses directory read restrictions");
    }

    auto const res = importer.importFromYamlOffline(yamlPath);

    REQUIRE(!res);
    CHECK(res.error().code == Error::Code::IoError);
  }

  TEST_CASE("LibraryYaml - merge publishes truthful inserted and mutated track ids",
            "[runtime][integration][import-export][changeset]")
  {
    auto const temp = ao::test::TempDir{};
    auto existingId = kInvalidTrackId;
    {
      auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());
      existingId = library::test::addTrackWithUniqueFixtureUri(
        ml, library::test::TrackSpec{.title = "Before", .artist = "", .album = "", .uri = "existing.flac"});
    }

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

    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto& executor = *executorPtr;
    auto core = ao::test::requireValue(CoreRuntime::create(
      std::move(executorPtr), temp.path(), temp.path(), {}, library::test::kTestMusicLibraryMapBytes));
    auto const& ml = core.musicLibrary();
    auto const& changes = core.library().changes();
    auto observed = std::vector<LibraryChangeSet>{};
    auto subscription =
      changes.onChanged([&observed](LibraryChangeSet const& value) noexcept { observed.push_back(value); });
    REQUIRE(importThroughRuntime(core, executor, yamlPath, ImportMode::Merge));

    REQUIRE(observed.size() == 1);
    REQUIRE(observed.front().tracksInserted.size() == 1);
    CHECK(observed.front().tracksMutated == std::vector{existingId});
    CHECK_FALSE(observed.front().libraryReset);
    auto transaction = ml.readTransaction();
    CHECK(observed.front().libraryRevision == ml.libraryRevision(transaction));
    auto const optInserted = ml.manifest().reader(transaction).get("inserted.flac");
    auto const optExisting = ml.manifest().reader(transaction).get("existing.flac");
    REQUIRE(optInserted);
    REQUIRE(optExisting);
    CHECK(optInserted->trackId() != existingId);
    CHECK(optExisting->trackId() == existingId);
    CHECK(observed.front().tracksInserted == std::vector{optInserted->trackId()});
    auto const optInsertedTrack = ml.tracks().reader(transaction).get(optInserted->trackId());
    auto const optExistingTrack = ml.tracks().reader(transaction).get(existingId);
    REQUIRE(optInsertedTrack);
    REQUIRE(optExistingTrack);
    CHECK(optInsertedTrack->metadata().title() == "Inserted");
    CHECK(optExistingTrack->metadata().title() == "After");
  }

  TEST_CASE("LibraryYaml - restore publishes a library reset", "[runtime][integration][import-export][changeset]")
  {
    auto const temp = ao::test::TempDir{};
    auto const yamlPath = std::filesystem::path{temp.path()} / "restore.yaml";
    {
      auto yaml = std::ofstream{yamlPath};
      yaml << "version: 5\nexport_mode: full\nlibrary:\n  resources: []\n  tracks: []\n  lists: []\n";
    }

    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto& executor = *executorPtr;
    auto core = ao::test::requireValue(CoreRuntime::create(
      std::move(executorPtr), temp.path(), temp.path(), {}, library::test::kTestMusicLibraryMapBytes));
    auto const& ml = core.musicLibrary();
    auto const& changes = core.library().changes();
    auto observed = std::vector<LibraryChangeSet>{};
    auto subscription =
      changes.onChanged([&observed](LibraryChangeSet const& value) noexcept { observed.push_back(value); });
    REQUIRE(importThroughRuntime(core, executor, yamlPath, ImportMode::Restore));

    REQUIRE(observed.size() == 1);
    CHECK(observed.front().libraryReset);
    auto transaction = ml.readTransaction();
    CHECK(observed.front().libraryRevision == ml.libraryRevision(transaction));
  }

  TEST_CASE("LibraryYaml - restore commits library id and content under one revision",
            "[runtime][integration][import-export][changeset]")
  {
    auto const temp = ao::test::TempDir{};
    auto const yamlPath = std::filesystem::path{temp.path()} / "restore-with-id.yaml";
    {
      auto yaml = std::ofstream{yamlPath};
      yaml << R"(version: 5
libraryId: 123E4567-E89B-12D3-A456-426614174000
export_mode: full
library:
  resources: []
  tracks:
    - uri: restored.flac
      title: Restored
  lists:
    - id: 42
      name: Restored List
      order:
        - uri: restored.flac
)";
    }

    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto& executor = *executorPtr;
    auto core = ao::test::requireValue(CoreRuntime::create(
      std::move(executorPtr), temp.path(), temp.path(), {}, library::test::kTestMusicLibraryMapBytes));
    auto const& ml = core.musicLibrary();
    auto const& changes = core.library().changes();
    auto observed = std::vector<LibraryChangeSet>{};
    auto subscription =
      changes.onChanged([&observed](LibraryChangeSet const& value) noexcept { observed.push_back(value); });
    auto const revisionBefore = core.library().snapshot().revision();
    REQUIRE(importThroughRuntime(core, executor, yamlPath, ImportMode::Restore));

    REQUIRE(observed.size() == 1);
    CHECK(observed.front().libraryReset);
    auto transaction = ml.readTransaction();
    CHECK(utility::formatUuid(ml.metadataHeader(transaction).libraryId) == "123e4567-e89b-12d3-a456-426614174000");
    CHECK(observed.front().libraryRevision == revisionBefore + 1);
    CHECK(ml.libraryRevision(transaction) == revisionBefore + 1);
    auto const optManifest = ml.manifest().reader(transaction).get("restored.flac");
    REQUIRE(optManifest);
    auto const trackReader = ml.tracks().reader(transaction);
    CHECK(trackReader.entryCount() == 1);
    auto const optTrack = trackReader.get(optManifest->trackId());
    REQUIRE(optTrack);
    CHECK(optTrack->property().uri() == "restored.flac");
    CHECK(optTrack->metadata().title() == "Restored");
    auto const listReader = ml.lists().reader(transaction);
    auto listIterator = listReader.begin();
    REQUIRE(listIterator != listReader.end());
    auto const [listId, list] = *listIterator;
    CHECK(listId != kInvalidListId);
    CHECK(list.name() == "Restored List");
    CHECK(std::ranges::equal(list.orderTrackIds(), std::array{optManifest->trackId()}));
    ++listIterator;
    CHECK(listIterator == listReader.end());
  }

  TEST_CASE("LibraryYaml - preview preserves library id and revision", "[runtime][integration][import-export][dry-run]")
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

    auto afterTransaction = ml.readTransaction();
    CHECK(ml.metadataHeader(afterTransaction).libraryId == originalLibraryId);
    CHECK(ml.libraryRevision(afterTransaction) == originalRevision);
  }
} // namespace ao::rt::test
