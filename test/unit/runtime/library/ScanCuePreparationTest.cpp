// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/ScanApplyOperation.h"
#include "test/unit/FilesystemTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/runtime/library/ScanApplyTestSupport.h"
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/ScanPlan.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    constexpr auto kSheet = "FILE ignored.wav WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"
                            "TRACK 02 AUDIO\nINDEX 01 00:00:01\n";

    void writeCue(std::filesystem::path const& path, std::string_view text)
    {
      auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
      stream << text;
      REQUIRE(stream.good());
    }
  } // namespace

  TEST_CASE("ScanApplyOperation - filesystem errors retain the scan failure stage",
            "[runtime][integration][library-scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = temp.path() / "music";
    auto const locked = musicRoot / "locked";
    std::filesystem::create_directories(locked);
    auto library = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");
    auto const access = ao::test::ScopedDirectoryAccessGuard{locked, ao::test::DeniedDirectoryAccess::Read};

    if (!access.isEffective())
    {
      SKIP("The current process can bypass directory access restrictions");
    }

    auto planRes = LibraryScan{library}.buildPlan();
    REQUIRE(planRes);
    REQUIRE(planRes->size() == 1);
    CHECK(planRes->items().front().classification == ScanClassification::Error);
    CHECK(planRes->items().front().cueDiagnostic == ScanCueDiagnostic::None);
    auto stages = std::vector<std::string>{};
    auto uris = std::vector<std::string>{};
    auto operation = ScanApplyOperation{library,
                                        std::move(*planRes),
                                        {},
                                        [&](ScanFailure const& failure)
                                        {
                                          stages.emplace_back(failure.stage);
                                          uris.emplace_back(failure.uri);
                                        }};
    auto res = operation.run();
    REQUIRE(res);
    CHECK(res->failureCount == 1);
    CHECK(res->insertedIds.empty());
    CHECK(stages == std::vector<std::string>{"scan"});
    CHECK(uris == std::vector<std::string>{"locked"});
  }

  TEST_CASE("ScanApplyOperation - malformed sheets retain the CUE binding failure stage",
            "[runtime][integration][library-scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = temp.path() / "music";
    std::filesystem::create_directories(musicRoot);
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), musicRoot / "disc.flac");
    writeCue(musicRoot / "disc.cue", "not a CUE sheet");
    auto library = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");
    auto planRes = LibraryScan{library}.buildPlan();
    REQUIRE(planRes);
    REQUIRE(planRes->size() == 1);
    CHECK(planRes->items().front().cueDiagnostic == ScanCueDiagnostic::SheetUnreadableOrMalformed);
    auto stages = std::vector<std::string>{};
    auto operation = ScanApplyOperation{
      library, std::move(*planRes), {}, [&](ScanFailure const& failure) { stages.emplace_back(failure.stage); }};
    auto res = operation.run();
    REQUIRE(res);
    CHECK(res->failureCount == 1);
    CHECK(res->insertedIds.empty());
    CHECK(stages == std::vector<std::string>{"bind CUE sheet for"});
  }

  TEST_CASE("ScanApplyOperation - a folder lost after planning fails only its own items",
            "[runtime][integration][library-scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = temp.path() / "music";

    for (auto const* folder : {"kept", "vanished", "unlistable"})
    {
      std::filesystem::create_directories(musicRoot / folder);
      std::filesystem::copy_file(
        audio::test::requireAudioFixture("basic_metadata.flac"), musicRoot / folder / "disc.flac");
    }

    auto library = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");
    auto planRes = LibraryScan{library}.buildPlan();
    REQUIRE(planRes);
    REQUIRE(planRes->count(ScanClassification::New) == 3);
    std::filesystem::remove_all(musicRoot / "vanished");
    auto const access =
      ao::test::ScopedDirectoryAccessGuard{musicRoot / "unlistable", ao::test::DeniedDirectoryAccess::List};

    if (!access.isEffective())
    {
      SKIP("The current process can bypass directory access restrictions");
    }

    auto stagesByUri = std::map<std::string, std::string>{};
    auto operation =
      ScanApplyOperation{library,
                         std::move(*planRes),
                         {},
                         [&](ScanFailure const& failure) { stagesByUri.emplace(failure.uri, failure.stage); }};
    auto res = operation.run();
    REQUIRE(res);
    CHECK(res->failureCount == 2);
    CHECK(res->insertedIds.size() == 1);
    CHECK(stagesByUri.size() == 2);
    CHECK(stagesByUri.contains("vanished/disc.flac"));
    CHECK(stagesByUri["unlistable/disc.flac"] == "read CUE directory");
    auto transaction = library.readTransaction();
    CHECK(library.manifest().reader(transaction).get("kept/disc.flac"));
    CHECK_FALSE(library.manifest().reader(transaction).get("unlistable/disc.flac"));
  }

  TEST_CASE("ScanApplyOperation - preparation shares a directory snapshot and final revalidation rereads it",
            "[runtime][integration][library-scan][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = temp.path() / "music";
    std::filesystem::create_directories(musicRoot);

    for (auto const* name : {"first", "second"})
    {
      std::filesystem::copy_file(
        audio::test::requireAudioFixture("basic_metadata.flac"), musicRoot / (std::string{name} + ".flac"));
      writeCue(musicRoot / (std::string{name} + ".cue"), kSheet);
    }

    auto library = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");
    auto planRes = LibraryScan{library}.buildPlan();
    REQUIRE(planRes);
    REQUIRE(planRes->count(ScanClassification::New) == 2);
    bool editedDuringPreparation = false;
    auto operation =
      ScanApplyOperation{library,
                         std::move(*planRes),
                         [&](ScanApplyProgress const& progress)
                         {
                           if (progress.stage == ScanApplyProgressStage::Updating && progress.itemIndex == 1)
                           {
                             writeCue(musicRoot / "first.cue", "changed after the directory snapshot");
                             writeCue(musicRoot / "second.cue", "changed after the directory snapshot");
                             editedDuringPreparation = true;
                           }
                         },
                         {}};
    auto preparedRes = operation.prepare();
    REQUIRE(preparedRes);
    REQUIRE(editedDuringPreparation);
    // Every item uses this phase's snapshot; the separate final read must
    // reject both changed sheets rather than publish either stale carrier.
    CHECK(preparedRes->staleCount == 0);
    requireRevalidation(operation, 2, 0);
    auto res = operation.run();
    REQUIRE(res);
    CHECK(res->staleCount == 2);
    CHECK(res->failureCount == 0);
    CHECK(res->insertedIds.empty());
    auto transaction = library.readTransaction();
    CHECK_FALSE(library.manifest().reader(transaction).get("first.flac"));
    CHECK_FALSE(library.manifest().reader(transaction).get("second.flac"));
    auto tracks = library.tracks().reader(transaction);
    CHECK(tracks.begin() == tracks.end());
  }
} // namespace ao::rt::test
