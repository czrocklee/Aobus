// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/library/LibraryYamlExporter.h"
#include "runtime/library/LibraryYamlImporter.h"
#include "test/FilesystemTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureDirectory.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/library/WritableLibraryTestSupport.h"
#include "test/runtime/AsyncTestSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
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
#include <ao/library/TrackView.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibraryImportPlan.h>
#include <ao/rt/library/LibraryJobs.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/library/LibraryTransfer.h>
#include <ao/yaml/RymlAdapter.h>

#include <c4/yml/tree.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
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

    void writeDeltaDocument(std::filesystem::path const& path, std::string_view trackRecords)
    {
      auto output = std::ofstream{path};
      output << "version: 1\nexport-mode: delta\nlibrary:\n  tracks:\n" << trackRecords << "  lists: []\n";
      output.close();
      REQUIRE(output);
    }

    ryml::ConstNodeRef findExportedTitle(ryml::Tree const& tree, std::string_view uri)
    {
      CAPTURE(uri);
      auto const tracks = tree.rootref()["library"]["tracks"];
      REQUIRE(tracks.is_seq());

      for (auto const track : tracks.children())
      {
        if (yaml::scalarView(track["uri"]) == uri)
        {
          return yaml::findChild(track, "title");
        }
      }

      FAIL("Missing exported Track");
    }

    void checkTitleScalar(ryml::ConstNodeRef const& title, std::string_view text)
    {
      REQUIRE(title.readable());
      REQUIRE(title.has_val());
      CHECK(yaml::scalarView(title) == text);
    }

    void checkTitleClear(ryml::ConstNodeRef const& title)
    {
      REQUIRE(title.readable());
      REQUIRE(title.is_map());
      REQUIRE(title.num_children() == 1);
      auto const action = title.first_child();
      CHECK(yaml::keyView(action) == "clear");
      checkTitleScalar(action, "true");
    }

    void checkStoredTitle(MusicLibrary const& library, std::string_view uri, std::string_view title, bool cleared)
    {
      CAPTURE(uri);
      auto transaction = library.readTransaction();
      auto const optEntry = library.manifest().reader(transaction).get(uri);
      REQUIRE(optEntry);
      auto const optTrack = library.tracks().reader(transaction).get(optEntry->trackId());
      REQUIRE(optTrack);
      CHECK(optTrack->metadata().title() == title);
      CHECK(optTrack->metadata().isTitleExplicitlyCleared() == cleared);
    }

    Result<ImportReport> importThroughRuntime(CoreRuntime& core,
                                              QueuedExecutor& executor,
                                              std::filesystem::path const& path,
                                              ImportMode mode)
    {
      auto& runtime = core.async();
      auto& jobs = core.libraryJobs();
      INFO("Preparing library import");
      auto planRes = runQueuedTask(runtime, executor, jobs.prepareLibraryImportAsync(path, mode), kImportTaskTimeout);

      if (!planRes)
      {
        return std::unexpected{planRes.error()};
      }

      INFO("Applying prepared library import");
      return runQueuedTask(
        runtime, executor, jobs.applyLibraryImportPlanAsync(std::move(*planRes)), kImportTaskTimeout);
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
                            core.libraryJobs().prepareLibraryImportAsync(
                              std::filesystem::path{temp.path()} / "absent.yaml", ImportMode::Merge))));
      executor.checkQueued();
      CHECK_FALSE(completedPtr->load());
      // Model a failed test assertion while the real import is suspended at its first executor hop.
      throw std::runtime_error{"import wait failed"};
    };
    REQUIRE_THROWS_AS(run(), std::runtime_error);
    REQUIRE(completedPtr->load());
    REQUIRE(optFuture);
    CHECK_THROWS_AS(optFuture->get(), async::OperationCancelled);
    auto reopened = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    CHECK(library::WritableMusicLibrary::acquire(reopened));
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

    std::filesystem::copy_file(
      audio::test::audioFixtureDirectory() / "with_cover.flac", std::filesystem::path{temp.path()} / "cover.flac");
    std::filesystem::copy_file(audio::test::audioFixtureDirectory() / "with_cover.flac",
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

  TEST_CASE("LibraryYaml - only an explicit title clear survives an offline delta relay")
  {
    auto const sourceTemp = ao::test::TempDir{};
    auto const& root = sourceTemp.path();
    auto const clearedUri = audio::test::installAudioFixture(root, "basic_metadata.mp3", "cleared.mp3");
    auto const untitledUri = audio::test::installAudioFixture(root, "empty.mp3", "untitled.mp3");

    {
      auto source = library::test::makeTestMusicLibrary(root, root);
      auto changes = makeStateOnlyLibraryChanges(source);
      auto commands = LibraryCommandsFixture{source, changes};
      auto const titled =
        ao::test::requireValue(commands.runTask(commands.commands().createTrackFromFileAsync(root / clearedUri)));
      auto const untitled =
        ao::test::requireValue(commands.runTask(commands.commands().createTrackFromFileAsync(root / untitledUri)));
      REQUIRE(titled.title == "Test Title");
      REQUIRE(untitled.title.empty());
      REQUIRE(commands.updateMetadata(std::array{titled.trackId}, MetadataPatch{.optTitle = ""}));
    }

    auto const overlayPath = root / "first-hop.yaml";
    {
      auto source = library::test::makeTestMusicLibrary(root, root);
      checkStoredTitle(source, clearedUri, "", true);
      checkStoredTitle(source, untitledUri, "", false);
      auto exporter = LibraryYamlExporter{source};
      auto const probePath = root / "baseline.yaml";

      // Change only the media, not the stored titles. With two blank file
      // baselines, the ordinary blank has no difference to export.
      audio::test::installAudioFixture(root, "empty.mp3", clearedUri);
      REQUIRE(exporter.exportToYaml(probePath, ExportMode::Delta));
      {
        auto buffer = std::vector<char>{};
        auto const tree = loadTree(probePath, buffer);
        checkTitleClear(findExportedTitle(tree, clearedUri));
        CHECK_FALSE(findExportedTitle(tree, untitledUri).readable());
      }

      // A now-titled file baseline makes the same ordinary blank a delta.
      audio::test::installAudioFixture(root, "basic_metadata.mp3", untitledUri);
      REQUIRE(exporter.exportToYaml(overlayPath, ExportMode::Delta));
      auto buffer = std::vector<char>{};
      auto const tree = loadTree(overlayPath, buffer);
      checkTitleClear(findExportedTitle(tree, clearedUri));
      checkTitleScalar(findExportedTitle(tree, untitledUri), "");
    }

    auto const mergeIntoRecipient =
      [&](std::filesystem::path const& targetRoot, std::filesystem::path const& input, std::string const& localTitle)
    {
      auto target = library::test::makeTestMusicLibrary(targetRoot, targetRoot);
      library::test::addTrack(target, library::test::TrackSpec{.title = localTitle, .uri = clearedUri});
      library::test::addTrack(target, library::test::TrackSpec{.title = localTitle, .uri = untitledUri});
      auto const reportRes = LibraryYamlImporter{target}.importFromYamlOffline(input, ImportMode::Merge);
      REQUIRE(reportRes);
      CHECK(reportRes->tracksCreated == 0);
      CHECK(reportRes->tracksUpdated == 2);
    };

    auto const relayTemp = ao::test::TempDir{};
    auto const& relayRoot = relayTemp.path();
    auto const relayedPath = relayRoot / "second-hop.yaml";
    mergeIntoRecipient(relayRoot, overlayPath, "Relay title");
    {
      auto relay = library::test::makeTestMusicLibrary(relayRoot, relayRoot);
      checkStoredTitle(relay, clearedUri, "", true);
      checkStoredTitle(relay, untitledUri, "", false);

      // Neither file exists at the relay, so its ordinary blank is equal to
      // the fallback baseline and only the clear is exported again.
      REQUIRE(LibraryYamlExporter{relay}.exportToYaml(relayedPath, ExportMode::Delta));
      auto buffer = std::vector<char>{};
      auto const tree = loadTree(relayedPath, buffer);
      checkTitleClear(findExportedTitle(tree, clearedUri));
      CHECK_FALSE(findExportedTitle(tree, untitledUri).readable());
    }

    auto const recipientTemp = ao::test::TempDir{};
    auto const& recipientRoot = recipientTemp.path();
    audio::test::installAudioFixture(recipientRoot, "basic_metadata.mp3", untitledUri);
    mergeIntoRecipient(recipientRoot, relayedPath, "Recipient title");
    auto recipient = library::test::makeTestMusicLibrary(recipientRoot, recipientRoot);
    checkStoredTitle(recipient, clearedUri, "", true);
    checkStoredTitle(recipient, untitledUri, "Recipient title", false);
  }

  TEST_CASE("LibraryYaml - a title clear survives backup restore and missing-media delta export")
  {
    auto const sourceTemp = ao::test::TempDir{};
    auto const& sourceRoot = sourceTemp.path();
    auto const seedPath = sourceRoot / "seed.yaml";
    writeDeltaDocument(seedPath, R"(    - uri: cleared.mp3
      title: {clear: true}
    - uri: untitled.mp3
      title: ""
)");
    auto source = library::test::makeTestMusicLibrary(sourceRoot, sourceRoot);
    REQUIRE(LibraryYamlImporter{source}.importFromYamlOffline(seedPath, ImportMode::Restore));
    checkStoredTitle(source, "cleared.mp3", "", true);
    checkStoredTitle(source, "untitled.mp3", "", false);

    for (auto const mode : std::array{ExportMode::Full, ExportMode::Metadata})
    {
      CAPTURE(mode);
      auto const backupPath = sourceRoot / "backup.yaml";
      REQUIRE(LibraryYamlExporter{source}.exportToYaml(backupPath, mode));
      {
        auto buffer = std::vector<char>{};
        auto const tree = loadTree(backupPath, buffer);
        checkTitleClear(findExportedTitle(tree, "cleared.mp3"));
        CHECK_FALSE(findExportedTitle(tree, "untitled.mp3").readable());
      }

      auto const targetTemp = ao::test::TempDir{};
      auto const& targetRoot = targetTemp.path();
      // Metadata restore seeds technical facts from this file, not its title
      // or a clear instruction for the omitted title.
      auto const uri = audio::test::installAudioFixture(targetRoot, "basic_metadata.mp3", "untitled.mp3");
      {
        auto target = library::test::makeTestMusicLibrary(targetRoot, targetRoot);
        REQUIRE(LibraryYamlImporter{target}.importFromYamlOffline(backupPath, ImportMode::Restore));
      }

      REQUIRE(std::filesystem::remove(targetRoot / uri));
      auto target = library::test::makeTestMusicLibrary(targetRoot, targetRoot);
      checkStoredTitle(target, "cleared.mp3", "", true);
      checkStoredTitle(target, "untitled.mp3", "", false);
      auto const deltaPath = targetRoot / "restored.yaml";
      REQUIRE(LibraryYamlExporter{target}.exportToYaml(deltaPath, ExportMode::Delta));
      auto buffer = std::vector<char>{};
      auto const tree = loadTree(deltaPath, buffer);
      checkTitleClear(findExportedTitle(tree, "cleared.mp3"));
      CHECK_FALSE(findExportedTitle(tree, "untitled.mp3").readable());
    }
  }

#ifdef _WIN32
  TEST_CASE("LibraryYaml - relays portable Windows records through delta and metadata without a disk baseline")
  {
    constexpr auto kUri = "albums/CON.flac";
    auto const temp = ao::test::TempDir{};
    auto const sourceRoot = temp.path() / "source-music";
    REQUIRE(std::filesystem::create_directory(sourceRoot));
    auto const fullPath = temp.path() / "full.yaml";
    {
      auto output = std::ofstream{fullPath};
      output << R"(version: 1
export-mode: full
library:
  resources: []
  tracks:
    - uri: albums/CON.flac
      title: Portable title
      year: 1984
      sample-rate: 96000
      file-size: 345
      mtime: null
  lists: []
)";
      output.close();
      REQUIRE(output);
    }

    auto source = library::test::makeTestMusicLibrary(sourceRoot, temp.path() / "source-db");
    REQUIRE(LibraryYamlImporter{source}.importFromYamlOffline(fullPath, ImportMode::Restore));
    auto const exportPath = temp.path() / "relay.yaml";

    for (auto const mode : std::array{ExportMode::Delta, ExportMode::Metadata})
    {
      CAPTURE(mode);
      auto const exportRes = LibraryYamlExporter{source}.exportToYaml(exportPath, mode);
      INFO((exportRes ? "portable record exported" : exportRes.error().message));
      REQUIRE(exportRes);
      auto buffer = std::vector<char>{};
      auto const tree = loadTree(exportPath, buffer);
      checkTitleScalar(findExportedTitle(tree, kUri), "Portable title");

      auto const recipientTemp = ao::test::TempDir{};
      auto const targetRoot = recipientTemp.path() / "music";
      REQUIRE(std::filesystem::create_directory(targetRoot));
      auto target = library::test::makeTestMusicLibrary(targetRoot, recipientTemp.path() / "db");
      auto const importRes = LibraryYamlImporter{target}.importFromYamlOffline(exportPath, ImportMode::Restore);
      INFO((importRes ? "portable record restored" : importRes.error().message));
      REQUIRE(importRes);
      CHECK(importRes->payloadMode == mode);
      CHECK(importRes->tracksCreated == 1);
      CHECK(std::filesystem::is_empty(targetRoot));

      auto transaction = target.readTransaction();
      auto const optEntry = target.manifest().reader(transaction).get(kUri);
      REQUIRE(optEntry);
      CHECK(optEntry->fileSize() == 0);
      CHECK_FALSE(optEntry->mtime());
      auto const optTrack =
        target.tracks().reader(transaction).get(optEntry->trackId(), TrackStore::Reader::LoadMode::Both);
      REQUIRE(optTrack);
      CHECK(optTrack->property().uri() == kUri);
      CHECK(optTrack->property().sampleRate() == SampleRate{});
      CHECK(optTrack->metadata().title() == "Portable title");
      CHECK(optTrack->metadata().year() == 1984);
    }
  }
#endif

  TEST_CASE("LibraryYaml - runtime merge of full and metadata backups clears only the explicitly named title")
  {
    for (auto const mode : std::array{ExportMode::Full, ExportMode::Metadata})
    {
      CAPTURE(mode);
      auto const temp = ao::test::TempDir{};
      auto const& root = temp.path();
      auto const backup = root / "backup.yaml";
      {
        auto yaml = std::ofstream{backup};
        yaml << "version: 1\nexport-mode: " << exportModeName(mode) << "\nlibrary:\n";

        if (mode == ExportMode::Full)
        {
          yaml << "  resources: []\n";
        }

        yaml << "  tracks:\n    - uri: clear.mp3\n      title: {clear: true}\n    - uri: keep.mp3\n  lists: []\n";
        yaml.close();
        REQUIRE(yaml);
      }
      {
        auto target = library::test::makeTestMusicLibrary(root, root);
        library::test::addTrack(target, {.title = "Clear me", .uri = "clear.mp3"});
        library::test::addTrack(target, {.title = "Keep me", .uri = "keep.mp3"});
      }

      auto executorPtr = std::make_unique<QueuedExecutor>();
      auto& executor = *executorPtr;
      auto core = ao::test::requireValue(
        CoreRuntime::create(std::move(executorPtr), root, root, {}, library::test::kTestMusicLibraryMapBytes));
      auto const reportRes = importThroughRuntime(core, executor, backup, ImportMode::Merge);
      REQUIRE(reportRes);
      CHECK(reportRes->payloadMode == mode);
      CHECK(reportRes->tracksCreated == 0);
      CHECK(reportRes->tracksUpdated == 2);
      checkStoredTitle(core.musicLibrary(), "clear.mp3", "", true);
      checkStoredTitle(core.musicLibrary(), "keep.mp3", "Keep me", false);
    }
  }

  TEST_CASE("LibraryYaml - omission preserves a title clear and scalar text replaces it on merge")
  {
    auto const temp = ao::test::TempDir{};
    auto const& root = temp.path();
    auto const overlayPath = root / "overlay.yaml";
    auto target = library::test::makeTestMusicLibrary(root, root);
    library::test::addTrack(target, library::test::TrackSpec{.title = "Before", .uri = "song.mp3"});

    auto const merge = [&](std::string_view fields)
    {
      writeDeltaDocument(overlayPath, "    - uri: song.mp3\n" + std::string{fields});
      REQUIRE(LibraryYamlImporter{target}.importFromYamlOffline(overlayPath, ImportMode::Merge));
    };

    merge("      title: {clear: \"true\"}\n");
    checkStoredTitle(target, "song.mp3", "", true);

    SECTION("omission preserves the clear")
    {
      merge("      year: 1984\n");
      checkStoredTitle(target, "song.mp3", "", true);
    }

    constexpr auto kTitles = std::to_array<std::pair<std::string_view, std::string_view>>({
      {"", ""},
      {" ''", ""},
      {" \"\"", ""},
      {" null", "null"},
      {" ~", "~"},
      {" '  '", "  "},
      {" '{clear: true}'", "{clear: true}"},
    });

    for (auto const& [scalar, text] : kTitles)
    {
      DYNAMIC_SECTION("scalar title:" << scalar)
      {
        merge("      title:" + std::string{scalar} + "\n");
        checkStoredTitle(target, "song.mp3", text, false);

        auto const exportedPath = root / "export.yaml";
        REQUIRE(LibraryYamlExporter{target}.exportToYaml(exportedPath, ExportMode::Delta));
        auto buffer = std::vector<char>{};
        auto const tree = loadTree(exportedPath, buffer);

        if (auto const title = findExportedTitle(tree, "song.mp3"); text.empty())
        {
          CHECK_FALSE(title.readable());
        }
        else
        {
          checkTitleScalar(title, text);
        }
      }
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

  TEST_CASE("LibraryYaml - delta and metadata import report filesystem inspection errors")
  {
    auto const mode = GENERATE(ExportMode::Delta, ExportMode::Metadata);
    CAPTURE(mode);
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
      yaml << "version: 1\nexport-mode: " << exportModeName(mode) << "\nlibrary:\n"
           << "  tracks:\n"
           << "    - uri: \"blocked/song.flac\"\n"
           << "      title: Cannot inspect baseline\n"
           << "  lists: []\n";
      yaml.close();
      REQUIRE(yaml);
    }

    auto const denied = ao::test::ScopedDirectoryAccessGuard{blockedDir, ao::test::DeniedDirectoryAccess::Read};

    if (!denied.isEffective())
    {
      SKIP("the current process bypasses directory read restrictions");
    }

    auto const res = importer.importFromYamlOffline(yamlPath);

    REQUIRE(!res);
    CHECK(res.error().code == Error::Code::IoError);
    auto transaction = ml.readTransaction();
    CHECK_FALSE(ml.manifest().reader(transaction).get("blocked/song.flac"));
  }

  TEST_CASE("LibraryYaml - full restore skips media lookup with both file facts present")
  {
    constexpr auto kUri = "blocked/song.flac";
    auto const temp = ao::test::TempDir{};
    auto const root = temp.path() / "music";
    auto const blockedDir = root / "blocked";
    auto const blockedFile = blockedDir / "song.flac";
    REQUIRE(std::filesystem::create_directories(blockedDir));
    REQUIRE(std::ofstream{blockedFile}.good());
    auto const denied = ao::test::ScopedDirectoryAccessGuard{blockedDir, ao::test::DeniedDirectoryAccess::Read};

    if (!denied.isEffective())
    {
      SKIP("The current process bypasses directory read restrictions");
    }

    auto ec = std::error_code{};
    REQUIRE_FALSE(std::filesystem::is_regular_file(blockedFile, ec));
    REQUIRE(ec == std::errc::permission_denied);

    bool hasFileSize = true;
    bool hasMtime = true;

    SECTION("file size and explicit null modification time")
    {
    }

    SECTION("file size omitted")
    {
      hasFileSize = false;
    }

    SECTION("modification time omitted")
    {
      hasMtime = false;
    }

    auto const documentPath = temp.path() / "full.yaml";
    {
      auto yaml = std::ofstream{documentPath};
      yaml << "version: 1\nexport-mode: full\nlibrary:\n  resources: []\n  tracks:\n"
           << "    - uri: " << kUri << "\n      title: Restored\n";

      if (hasFileSize)
      {
        yaml << "      file-size: 345\n";
      }

      if (hasMtime)
      {
        yaml << "      mtime: null\n";
      }

      yaml << "  lists: []\n";
      yaml.close();
      REQUIRE(yaml);
    }

    auto ml = library::test::makeTestMusicLibrary(root, temp.path() / "db");
    auto const keptId = library::test::addTrack(ml, library::test::makeEmptyTrackSpec("kept.flac"));
    auto const revisionBefore = [&ml]
    {
      auto transaction = ml.readTransaction();
      return ml.libraryRevision(transaction);
    }();
    auto const res = LibraryYamlImporter{ml}.importFromYamlOffline(documentPath, ImportMode::Restore);
    auto transaction = ml.readTransaction();
    auto const tracks = ml.tracks().reader(transaction);
    auto const manifest = ml.manifest().reader(transaction);
    auto const optRestored = manifest.get(kUri);
    CHECK(tracks.entryCount() == 1);

    if (!hasFileSize || !hasMtime)
    {
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::IoError);
      CHECK(ml.libraryRevision(transaction) == revisionBefore);
      CHECK(tracks.get(keptId));
      CHECK_FALSE(optRestored);
      return;
    }

    REQUIRE(res);
    CHECK(res->tracksCreated == 1);
    CHECK_FALSE(tracks.get(keptId));
    REQUIRE(optRestored);
    CHECK(optRestored->fileSize() == 345);
    CHECK_FALSE(optRestored->mtime());
    auto const optTrack = tracks.get(optRestored->trackId());
    REQUIRE(optTrack);
    CHECK(optTrack->property().uri() == kUri);
    CHECK(optTrack->metadata().title() == "Restored");
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
