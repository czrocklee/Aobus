// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/ScanApplyOperation.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/library/ScanApplyTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/Credits.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/ScanPlan.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  using library::test::TrackSpec;
  using library::test::updateTrackSpec;

  TEST_CASE("ScanApplyOperation - new scans import explicit work ahead of conflicting grouping",
            "[runtime][integration][library-scan]")
  {
    auto comments = std::vector<std::string>{};
    auto expectedWork = std::string{};

    SECTION("WORK comment precedes GROUPING")
    {
      comments = {"TITLE=Scan Title", "WORK=Explicit Work", "GROUPING=Grouping Name"};
      expectedWork = "Explicit Work";
    }

    SECTION("GROUPING comment precedes WORK")
    {
      comments = {"TITLE=Scan Title", "GROUPING=Grouping Name", "WORK=Explicit Work"};
      expectedWork = "Explicit Work";
    }

    SECTION("grouping-only file keeps the grouping fallback")
    {
      comments = {"TITLE=Scan Title", "GROUPING=Grouping Name"};
      expectedWork = "Grouping Name";
    }

    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);
    writeScanFlacMetadataFixture(musicRoot / "song.flac", comments);

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    auto const trackId = importOne(ml);

    auto const spec = storedTrackSpec(ml, trackId);
    CHECK(spec.title == "Scan Title");
    CHECK(spec.work == expectedWork);
  }

  TEST_CASE("ScanApplyOperation - malformed explicit work is rejected instead of falling back to grouping",
            "[runtime][integration][library-scan][unicode]")
  {
    auto comments = std::vector<std::string>{"TITLE=Scan Title"};
    auto const malformedWork = std::string{"WORK="} + std::string{"\xC3\x28", 2};

    SECTION("malformed WORK precedes GROUPING")
    {
      comments.push_back(malformedWork);
      comments.emplace_back("GROUPING=Valid Grouping");
    }

    SECTION("GROUPING precedes malformed WORK")
    {
      comments.emplace_back("GROUPING=Valid Grouping");
      comments.push_back(malformedWork);
    }

    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);
    writeScanFlacMetadataFixture(musicRoot / "song.flac", comments);
    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    auto plan = LibraryScan{ml}.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::New) == 1);
    auto operation = ScanApplyOperation{ml, std::move(plan), {}, {}};
    auto const runRes = operation.run();

    REQUIRE(runRes);
    CHECK(runRes->failureCount == 1);
    CHECK(runRes->insertedIds.empty());
    CHECK(runRes->mutatedIds.empty());
    auto transaction = ml.readTransaction();
    CHECK(ml.tracks().reader(transaction).entryCount() == 0);
    CHECK_FALSE(ml.manifest().reader(transaction).get("song.flac"));
    CHECK(ml.dictionary().size() == 0);
  }

  TEST_CASE("ScanApplyOperation - changed-file scans keep the grouping-derived work of an existing track",
            "[runtime][integration][library-scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    // The first import carries only the grouping alias, so the stored work
    // has file origin through the grouping fallback.
    auto const scannedFile = musicRoot / "song.flac";
    writeScanFlacMetadataFixture(scannedFile, {"TITLE=Original Title", "GROUPING=Grouping Work"});

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    auto const trackId = importOne(ml);
    REQUIRE(storedTrackSpec(ml, trackId).work == "Grouping Work");

    // Retag the scanned file with explicit work and a different grouping, and
    // add an identical new file, so one scan observes a changed item and a new
    // item carrying the same tags.
    auto const retaggedFile = musicRoot / "retagged.flac";
    writeScanFlacMetadataFixture(
      retaggedFile, {"TITLE=Retagged Title", "WORK=Retagged Explicit Work", "GROUPING=Retagged Grouping"}, 88200);
    replaceFile(scannedFile, retaggedFile);

    auto plan = LibraryScan{ml}.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::Changed) == 1);
    REQUIRE(plan.count(ScanClassification::New) == 1);

    auto operation = ScanApplyOperation{ml, std::move(plan), {}, {}};
    auto const runRes = operation.run();
    REQUIRE(runRes);
    CHECK(runRes->failureCount == 0);
    REQUIRE(runRes->mutatedIds.size() == 1);
    CHECK(runRes->mutatedIds.front() == trackId);
    REQUIRE(runRes->insertedIds.size() == 1);

    // The existing track keeps its stored work and title: the scan refreshes
    // technical properties but does not correct the stored work to the value
    // the retagged file would now import.
    auto const changedSpec = storedTrackSpec(ml, trackId);
    CHECK(changedSpec.work == "Grouping Work");
    CHECK(changedSpec.title == "Original Title");
    CHECK(changedSpec.duration == std::chrono::seconds{2});

    // The same scan imports the retagged file as a new track through the
    // explicit-work precedence, so the preserved value is not a broken import
    // path.
    auto const newSpec = storedTrackSpec(ml, runRes->insertedIds.front());
    CHECK(newSpec.work == "Retagged Explicit Work");
    CHECK(newSpec.title == "Retagged Title");
  }

  TEST_CASE("ScanApplyOperation - changed-file scans keep curated work and credits over file tags",
            "[runtime][integration][library-scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const scannedFile = musicRoot / "song.flac";
    writeScanFlacMetadataFixture(scannedFile,
                                 {"TITLE=File Title",
                                  "WORK=File Work",
                                  "MOVEMENTNAME=File Movement",
                                  "CONDUCTOR=File Conductor",
                                  "ENSEMBLE=File Ensemble",
                                  "SOLOIST=File Soloist"});

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    auto const trackId = importOne(ml);

    updateTrackSpec(ml,
                    trackId,
                    [](TrackSpec& spec)
                    {
                      spec.work = "Curated Work";
                      spec.movement = "Curated Movement";
                      spec.credits = {
                        {.name = "Curated Conductor", .kind = library::CreditKind::Conductor},
                        {.name = "Curated Ensemble", .kind = library::CreditKind::Ensemble},
                        {.name = "Curated Soloist", .kind = library::CreditKind::Soloist, .role = "Violin"},
                      };
                      spec.movementNumber = 3;
                      spec.movementTotal = 8;
                    });

    // Retag a copy outside the music root with different classical tags,
    // including an explicit SOLOIST and a PERFORMER list comment, then swap
    // it in.
    auto const retaggedFile = std::filesystem::path{temp.path()} / "retagged.flac";
    writeScanFlacMetadataFixture(retaggedFile,
                                 {"TITLE=Retagged Title",
                                  "WORK=Retagged Work",
                                  "MOVEMENTNAME=Retagged Movement",
                                  "CONDUCTOR=Retagged Conductor",
                                  "ENSEMBLE=Retagged Ensemble",
                                  "SOLOIST=Retagged Soloist",
                                  "PERFORMER=Retagged Performer"},
                                 88200);
    replaceFile(scannedFile, retaggedFile);

    auto plan = LibraryScan{ml}.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::Changed) == 1);

    auto operation = ScanApplyOperation{ml, std::move(plan), {}, {}};
    auto const runRes = operation.run();
    REQUIRE(runRes);
    CHECK(runRes->failureCount == 0);
    REQUIRE(runRes->mutatedIds.size() == 1);
    CHECK(runRes->mutatedIds.front() == trackId);
    CHECK(runRes->insertedIds.empty());

    auto const spec = storedTrackSpec(ml, trackId);
    CHECK(spec.work == "Curated Work");
    CHECK(spec.movement == "Curated Movement");
    CHECK(spec.credits == std::vector<library::Credit>{
                            {.name = "Curated Conductor", .kind = library::CreditKind::Conductor},
                            {.name = "Curated Ensemble", .kind = library::CreditKind::Ensemble},
                            {.name = "Curated Soloist", .kind = library::CreditKind::Soloist, .role = "Violin"},
                          });
    CHECK(spec.movementNumber == 3);
    CHECK(spec.movementTotal == 8);
    CHECK(spec.duration == std::chrono::seconds{2});
  }
} // namespace ao::rt::test
