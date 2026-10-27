// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/ScanApplyOperation.h"
#include "test/FilesystemTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include "test/runtime/library/ScanApplyTestSupport.h"
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/ScanPlan.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    constexpr auto kPlannedSheet = "FILE ignored.wav WAVE\nTRACK 01 AUDIO\nTITLE First\nINDEX 01 00:00:00\n"
                                   "TRACK 02 AUDIO\nTITLE Second\nINDEX 01 00:01:00\n";
    constexpr auto kRetargetedSheet = "FILE ignored.wav WAVE\nTRACK 01 AUDIO\nTITLE First\nINDEX 01 00:00:00\n"
                                      "TRACK 02 AUDIO\nTITLE Second\nINDEX 01 00:00:30\n";

    void writePathSheet(std::filesystem::path const& path, std::string_view text)
    {
      auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
      output << text;
      REQUIRE(output.good());
    }

    void copyWithMtime(std::filesystem::path const& source, std::filesystem::path const& destination)
    {
      auto const time = std::filesystem::last_write_time(source);
      std::filesystem::copy_file(source, destination);
      std::filesystem::last_write_time(destination, time);
      REQUIRE(std::filesystem::file_size(destination) == std::filesystem::file_size(source));
      REQUIRE(std::filesystem::last_write_time(destination) == time);
    }
  } // namespace

  TEST_CASE("ScanApplyOperation - retargeted root alias leaves a planned new whole file stale",
            "[runtime][unit][library-scan][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto const plannedRoot = temp.path() / "planned";
    auto const destinationRoot = temp.path() / "destination";
    auto const configuredRoot = temp.path() / "music";
    std::filesystem::create_directories(plannedRoot);
    std::filesystem::create_directories(destinationRoot);
    writeScanFlacMetadataFixture(plannedRoot / "song.flac", {"TITLE=Planned whole file"}, 88200);
    auto const rootAlias = ao::test::SymlinkFixture{plannedRoot, configuredRoot, ao::test::SymlinkType::Directory};
    auto library = library::test::makeTestMusicLibrary(configuredRoot, temp.path() / "db");
    auto const revision = library.libraryRevision(library.readTransaction());
    auto planRes = LibraryScan{library}.buildPlan();
    REQUIRE(planRes);
    REQUIRE(planRes->size() == 1);
    auto const& item = planRes->items().front();
    REQUIRE(item.classification == ScanClassification::New);
    REQUIRE(item.uri == "song.flac");
    REQUIRE(item.fullPath == std::filesystem::canonical(plannedRoot / "song.flac"));
    REQUIRE(item.cueLayout.empty());
    REQUIRE(item.cueEvidence.general.empty());
    REQUIRE(item.cueEvidence.extensionSpecific.empty());

    // These files did not participate in planning. The destination's sheet
    // divides the same media that the saved directory classified as whole.
    copyWithMtime(plannedRoot / "song.flac", destinationRoot / "song.flac");
    writePathSheet(destinationRoot / "song.cue", kRetargetedSheet);
    bool retargetBeforePreparation = false;

    SECTION("retarget before preparation")
    {
      retargetBeforePreparation = true;
    }

    SECTION("retarget after preparation before final proof")
    {
    }

    auto failureUris = std::vector<std::string>{};
    auto operation = ScanApplyOperation{
      library, std::move(*planRes), {}, [&](ScanFailure const& failure) { failureUris.emplace_back(failure.uri); }};

    if (!retargetBeforePreparation)
    {
      requirePrepared(operation);
    }

    REQUIRE(std::filesystem::remove(configuredRoot));
    // SymlinkFixture explicitly SKIPs on hosts without symlink privilege.
    auto const retargetedAlias =
      ao::test::SymlinkFixture{destinationRoot, configuredRoot, ao::test::SymlinkType::Directory};
    REQUIRE(std::filesystem::canonical(configuredRoot / "song.flac") !=
            std::filesystem::canonical(plannedRoot / "song.flac"));
    REQUIRE(std::filesystem::file_size(configuredRoot / "song.flac") ==
            std::filesystem::file_size(plannedRoot / "song.flac"));
    REQUIRE(std::filesystem::last_write_time(configuredRoot / "song.flac") ==
            std::filesystem::last_write_time(plannedRoot / "song.flac"));

    if (retargetBeforePreparation)
    {
      auto const preparedRes = operation.prepare();
      REQUIRE(preparedRes);
      CHECK(preparedRes->staleCount == 1);
      CHECK(preparedRes->failureCount == 0);
    }

    auto const revalidatedRes = operation.revalidatePreparedFiles();
    REQUIRE(revalidatedRes);
    CHECK(revalidatedRes->staleCount == 1);
    CHECK(revalidatedRes->failureCount == 0);
    auto const res = operation.run();
    REQUIRE(res);
    CHECK(res->staleCount == 1);
    CHECK(res->failureCount == 0);
    CHECK(res->insertedIds.empty());
    CHECK(res->mutatedIds.empty());
    CHECK(res->relinkedIds.empty());
    CHECK(res->relinkedFileCount == 0);
    CHECK(res->missingCount == 0);
    CHECK(res->libraryRevision == 0);
    CHECK_FALSE(operation.shouldCommitTransaction());
    CHECK(failureUris.empty());
    CHECK(library.libraryRevision(library.readTransaction()) == revision);
    auto transaction = library.readTransaction();
    auto manifests = library.manifest().reader(transaction);
    CHECK(manifests.begin() == manifests.end());
    auto tracks = library.tracks().reader(transaction);
    CHECK(tracks.begin() == tracks.end());
  }

  TEST_CASE("ScanApplyOperation - retargeted moved carrier aborts every sibling without changing source form",
            "[runtime][integration][library-scan][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = temp.path() / "music";
    std::filesystem::create_directories(musicRoot);
    auto const original = musicRoot / "disc.flac";
    auto const moved = musicRoot / "renamed.flac";
    writeScanFlacMetadataFixture(original, {"TITLE=Carrier"}, 88200);
    writePathSheet(musicRoot / "disc.cue", kPlannedSheet);
    auto library = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");
    auto importPlanRes = LibraryScan{library}.buildPlan();
    REQUIRE(importPlanRes);
    auto const importedRes = ScanApplyOperation{library, std::move(*importPlanRes), {}, {}}.run();
    REQUIRE(importedRes);
    REQUIRE(importedRes->failureCount == 0);
    REQUIRE(importedRes->insertedIds.size() == 2);
    auto const ids = importedRes->insertedIds;
    auto const revision = library.libraryRevision(library.readTransaction());
    auto const sourceManifest = [&]
    {
      auto transaction = library.readTransaction();
      auto const optManifest = library.manifest().reader(transaction).get("disc.flac");
      REQUIRE(optManifest);
      REQUIRE(optManifest->segmentCount() == 2);
      REQUIRE(optManifest->audioPayloadLength() != 0);
      auto const bytes = optManifest->rawData();
      return std::vector<std::byte>{bytes.begin(), bytes.end()};
    }();
    std::filesystem::rename(original, moved);
    std::filesystem::rename(musicRoot / "disc.cue", musicRoot / "renamed.cue");
    // A distinct payload makes this sibling New rather than an ambiguous move.
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), musicRoot / "sibling.flac");
    auto planRes = LibraryScan{library}.buildPlan();
    REQUIRE(planRes);
    REQUIRE(planRes->size() == 2);
    REQUIRE(planRes->count(ScanClassification::Moved) == 1);
    REQUIRE(planRes->count(ScanClassification::New) == 1);
    auto const movedItem = std::ranges::find(planRes->items(), ScanClassification::Moved, &ScanItem::classification);
    REQUIRE(movedItem != planRes->items().end());
    REQUIRE(movedItem->uri == "renamed.flac");
    REQUIRE(movedItem->oldUri == "disc.flac");
    REQUIRE(movedItem->fullPath == std::filesystem::canonical(moved));
    auto const plannedPhysicalPath = movedItem->fullPath;
    REQUIRE(movedItem->errorMessage.empty());
    REQUIRE(movedItem->optManifestEvidence);
    REQUIRE(movedItem->optManifestEvidence->members.size() == 2);

    // Create the alternate physical destination only after the opaque plan.
    // Both copies have the same bytes, size, timestamp and payload identity;
    // only resolution provenance and the effective CUE directory will differ.
    auto const destinationRoot = musicRoot / "destination";
    std::filesystem::create_directories(destinationRoot);
    auto const destination = destinationRoot / "renamed.flac";
    copyWithMtime(moved, destination);
    writePathSheet(destinationRoot / "renamed.cue", kRetargetedSheet);
    std::filesystem::last_write_time(
      destinationRoot / "renamed.cue", std::filesystem::last_write_time(musicRoot / "renamed.cue"));
    bool retargetBeforePreparation = false;

    SECTION("retarget before preparation")
    {
      retargetBeforePreparation = true;
    }

    SECTION("retarget after preparation before final proof")
    {
    }

    auto failureUris = std::vector<std::string>{};
    auto failureStages = std::vector<std::string>{};
    auto failureMessages = std::vector<std::string>{};
    auto operation = ScanApplyOperation{library,
                                        std::move(*planRes),
                                        {},
                                        [&](ScanFailure const& failure)
                                        {
                                          failureUris.emplace_back(failure.uri);
                                          failureStages.emplace_back(failure.stage);
                                          failureMessages.emplace_back(failure.message);
                                        }};

    if (!retargetBeforePreparation)
    {
      requirePrepared(operation);
    }

    REQUIRE(std::filesystem::remove(moved));
    auto const fileAlias = ao::test::SymlinkFixture{destination, moved, ao::test::SymlinkType::File};
    REQUIRE(std::filesystem::canonical(moved) == std::filesystem::canonical(destination));
    REQUIRE(std::filesystem::canonical(moved) != plannedPhysicalPath);

    if (retargetBeforePreparation)
    {
      auto const preparedRes = operation.prepare();
      REQUIRE(preparedRes);
      CHECK(preparedRes->failureCount == 1);
      CHECK(preparedRes->staleCount == 0);
    }

    auto const revalidatedRes = operation.revalidatePreparedFiles();
    REQUIRE(revalidatedRes);
    CHECK(revalidatedRes->failureCount == 1);
    CHECK(revalidatedRes->staleCount == 0);
    CHECK_FALSE(operation.isReadyForMutation());
    auto const res = operation.run();
    REQUIRE(res);
    CHECK(res->failureCount == 1);
    CHECK(res->staleCount == 0);
    CHECK(res->insertedIds.empty());
    CHECK(res->mutatedIds.empty());
    CHECK(res->relinkedIds.empty());
    CHECK(res->relinkedFileCount == 0);
    CHECK(res->missingCount == 0);
    CHECK(res->libraryRevision == 0);
    CHECK_FALSE(operation.shouldCommitTransaction());
    CHECK(failureUris == std::vector<std::string>{"renamed.flac"});
    CHECK(failureStages.size() == 1);
    CHECK(failureMessages.size() == 1);

    for (auto const& stage : failureStages)
    {
      CHECK_FALSE(stage.empty());
    }

    for (auto const& message : failureMessages)
    {
      CHECK_FALSE(message.empty());
    }

    CHECK(library.libraryRevision(library.readTransaction()) == revision);
    auto transaction = library.readTransaction();
    auto manifests = library.manifest().reader(transaction);
    CHECK_FALSE(manifests.get("renamed.flac"));
    CHECK_FALSE(manifests.get("sibling.flac"));
    auto const optSource = manifests.get("disc.flac");
    REQUIRE(optSource);
    auto const bytes = optSource->rawData();
    CHECK(std::vector<std::byte>{bytes.begin(), bytes.end()} == sourceManifest);
    auto tracks = library.tracks().reader(transaction);
    CHECK(tracks.entryCount() == 2);

    for (auto const id : ids)
    {
      auto const optTrack = tracks.get(id);
      REQUIRE(optTrack);
      CHECK(optTrack->property().uri() == "disc.flac");
      REQUIRE(optTrack->property().segment());
      auto const segment = *optTrack->property().segment();
      CHECK(segment.frameCount == 44100);
      CHECK(optTrack->property().sampleRate() == 44100);

      if (id == ids.front())
      {
        CHECK(segment.ordinal == 1);
        CHECK(segment.startFrame == 0);
        CHECK(optTrack->metadata().title() == "First");
      }
      else
      {
        CHECK(segment.ordinal == 2);
        CHECK(segment.startFrame == 44100);
        CHECK(optTrack->metadata().title() == "Second");
      }
    }
  }
} // namespace ao::rt::test
