// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/library/LibraryYamlExporter.h"
#include "runtime/library/LibraryYamlImporter.h"
#include "test/FilesystemTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/library/WritableLibraryTestSupport.h"
#include "test/runtime/AsyncTestSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/async/TaskFuture.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListWriter.h>
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

  TEST_CASE("LibraryYaml - failed import wait retires the suspended task during runtime unwinding", "[concurrency]")
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

  TEST_CASE("LibraryYaml - delta export writes changed and unreadable tracks")
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

      // A delta document carries no embedded cover in version 1. The library holds
      // what the last scan saw, so a file retagged since then makes the two
      // differ, and the sequence delta would carry is the stale one: applying it
      // would overwrite the covers the baseline just read from the file with
      // references to content that may exist nowhere.
      CHECK_FALSE(tracks[2].has_child("covers"));
      CHECK_FALSE(tracks[3].has_child("covers"));
      CHECK_FALSE(root["library"].has_child("resources"));
    }
  }

  TEST_CASE("LibraryYaml - delta export reports filesystem inspection errors")
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

  TEST_CASE("LibraryYaml - delta import reports filesystem inspection errors")
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
      yaml << "version: 1\n"
           << "export-mode: delta\n"
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

  TEST_CASE("LibraryYaml - merge publishes truthful inserted and mutated track ids")
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
      yaml << R"(version: 1
export-mode: delta
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

  TEST_CASE("LibraryYaml - restore publishes a library reset")
  {
    auto const temp = ao::test::TempDir{};
    auto const yamlPath = std::filesystem::path{temp.path()} / "restore.yaml";
    {
      auto yaml = std::ofstream{yamlPath};
      yaml << "version: 1\nexport-mode: full\nlibrary:\n  resources: []\n  tracks: []\n  lists: []\n";
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

  TEST_CASE("LibraryYaml - restore commits content under one revision and keeps the library id")
  {
    auto const temp = ao::test::TempDir{};
    auto const yamlPath = std::filesystem::path{temp.path()} / "restore.yaml";
    {
      auto yaml = std::ofstream{yamlPath};
      yaml << R"(version: 1
export-mode: full
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
    auto const libraryIdBefore = ml.metadataHeader().libraryId;
    REQUIRE(importThroughRuntime(core, executor, yamlPath, ImportMode::Restore));

    REQUIRE(observed.size() == 1);
    CHECK(observed.front().libraryReset);
    auto transaction = ml.readTransaction();
    CHECK(ml.metadataHeader(transaction).libraryId == libraryIdBefore);
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

  TEST_CASE("LibraryYaml - restore gives restored Tracks and Lists fresh ids")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    auto const originalTrackId =
      library::test::addTrackWithUniqueFixtureUri(ml, library::test::makeEmptyTrackSpec("kept.flac"));
    auto const originalListId = [&ml, originalTrackId]
    {
      auto builder = library::ListBuilder::makeEmpty();
      builder.name("Kept").orderTrackIds().add(originalTrackId);
      auto transaction = library::test::writeTransaction(ml);
      auto const idRes =
        transaction.apply([&builder](library::LibraryWrite& write) { return write.lists().create(builder); });
      REQUIRE(idRes);
      REQUIRE(transaction.commit());
      return *idRes;
    }();

    auto const yamlPath = std::filesystem::path{temp.path()} / "backup.yaml";
    auto exporter = LibraryYamlExporter{ml};
    REQUIRE(exporter.exportToYaml(yamlPath, ExportMode::Full));
    auto importer = LibraryYamlImporter{ml};
    REQUIRE(importer.importFromYamlOffline(yamlPath, ImportMode::Restore));

    // A reference saved before the restore must dangle rather than name a
    // restored object, so neither old id may come back.
    auto transaction = ml.readTransaction();
    auto const optManifest = ml.manifest().reader(transaction).get("kept.flac");
    REQUIRE(optManifest);
    auto const restoredTrackId = optManifest->trackId();
    CHECK(restoredTrackId != originalTrackId);
    CHECK_FALSE(ml.tracks().reader(transaction).get(originalTrackId));

    auto const listReader = ml.lists().reader(transaction);
    CHECK_FALSE(listReader.get(originalListId));
    auto iterator = listReader.begin();
    REQUIRE(iterator != listReader.end());
    auto const [restoredListId, restoredList] = *iterator;
    CHECK(restoredListId != originalListId);
    CHECK(restoredList.name() == "Kept");
    CHECK(std::ranges::equal(restoredList.orderTrackIds(), std::array{restoredTrackId}));
    ++iterator;
    CHECK(iterator == listReader.end());
  }

  TEST_CASE("LibraryYaml - restore preview leaves revision, content, and id marks unchanged")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    auto const keptId = library::test::addTrackWithUniqueFixtureUri(ml, library::test::makeEmptyTrackSpec("kept.flac"));
    auto const yamlPath = std::filesystem::path{temp.path()} / "preview.yaml";
    {
      auto yaml = std::ofstream{yamlPath};
      yaml << "version: 1\n"
           << "export-mode: full\n"
           << "library:\n"
           << "  resources: []\n"
           << "  tracks:\n"
           << "    - uri: previewed.flac\n"
           << "  lists: []\n";
    }

    auto const originalRevision = [&ml]
    {
      auto transaction = ml.readTransaction();
      return ml.libraryRevision(transaction);
    }();
    auto importer = LibraryYamlImporter{ml};

    REQUIRE(importer.previewImportFromYamlOffline(yamlPath, ImportMode::Restore));

    {
      auto afterTransaction = ml.readTransaction();
      CHECK(ml.libraryRevision(afterTransaction) == originalRevision);
      auto const trackReader = ml.tracks().reader(afterTransaction);
      CHECK(trackReader.entryCount() == 1);
      CHECK(trackReader.get(keptId));
      CHECK_FALSE(ml.manifest().reader(afterTransaction).get("previewed.flac"));
    }

    // The preview allocated an id it never published, so the next Track
    // continues from the committed mark.
    CHECK(library::test::addTrackWithUniqueFixtureUri(ml, library::test::makeEmptyTrackSpec("next.flac")) ==
          TrackId{keptId.raw() + 1U});
  }
} // namespace ao::rt::test
