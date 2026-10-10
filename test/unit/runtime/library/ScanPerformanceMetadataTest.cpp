// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/MediaTrack.h"
#include "runtime/library/ScanApplyOperation.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/library/ScanApplyTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/Credits.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/FileTimeConversion.h>
#include <ao/library/RecordingDate.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/utility/Hash128.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  using library::test::TrackSpec;
  using library::test::updateTrackSpec;

  TEST_CASE("ScanApplyOperation - new scans import PERFORMER credits without inferring recording date or Soloist",
            "[runtime][integration][library-scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);
    writeScanFlacMetadataFixture(
      musicRoot / "song.flac",
      {"TITLE=Scan Title", "DATE=1981", "PERFORMER=Ada (Piano)", "PERFORMER=Bob", "PERFORMER=Ada (Violin)"});

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    auto const trackId = importOne(ml);

    auto const spec = storedTrackSpec(ml, trackId);
    CHECK(spec.title == "Scan Title");
    CHECK(spec.year == 1981);
    // A release date is not a recording date: no file source infers one.
    CHECK_FALSE(spec.recordingDate.isPresent());
    CHECK(spec.credits == std::vector<library::Credit>{
                            {.name = "Ada", .role = "Piano"}, {.name = "Bob"}, {.name = "Ada", .role = "Violin"}});
  }

  TEST_CASE("ScanApplyOperation - explicit SOLOIST stays independent of PERFORMER in either order",
            "[runtime][integration][library-scan]")
  {
    auto comments = std::vector<std::string>{};

    SECTION("SOLOIST precedes PERFORMER")
    {
      comments = {"TITLE=Scan Title", "SOLOIST=Anne", "PERFORMER=Ada (Piano)"};
    }

    SECTION("PERFORMER precedes SOLOIST")
    {
      comments = {"TITLE=Scan Title", "PERFORMER=Ada (Piano)", "SOLOIST=Anne"};
    }

    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);
    writeScanFlacMetadataFixture(musicRoot / "song.flac", comments);
    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    auto const trackId = importOne(ml);

    auto const spec = storedTrackSpec(ml, trackId);
    CHECK(spec.credits == std::vector<library::Credit>{
                            {.name = "Anne", .kind = library::CreditKind::Soloist}, {.name = "Ada", .role = "Piano"}});
  }

  TEST_CASE("ScanApplyOperation - new scans retain repeated kinds and whole-file nonblank Ensemble precedence",
            "[runtime][integration][library-scan]")
  {
    auto comments = std::vector<std::string>{};
    auto expectedEnsembles = std::vector<library::Credit>{};

    SECTION("explicit entries precede fallback")
    {
      comments = {"ENSEMBLE= Band ", "ENSEMBLE=Band", "ORCHESTRA=Fallback"};
      expectedEnsembles = {{.name = "Band", .kind = library::CreditKind::Ensemble},
                           {.name = "Band", .kind = library::CreditKind::Ensemble}};
    }

    SECTION("fallback precedes explicit entries")
    {
      comments = {"ORCHESTRA=Fallback", "ENSEMBLE= Band ", "ENSEMBLE=Band"};
      expectedEnsembles = {{.name = "Band", .kind = library::CreditKind::Ensemble},
                           {.name = "Band", .kind = library::CreditKind::Ensemble}};
    }

    SECTION("blank explicit entries do not suppress fallback")
    {
      comments = {"ENSEMBLE= \t", "ORCHESTRA=Fallback", "ORCHESTRA=Other fallback"};
      expectedEnsembles = {{.name = "Fallback", .kind = library::CreditKind::Ensemble},
                           {.name = "Other fallback", .kind = library::CreditKind::Ensemble}};
    }

    comments.insert(comments.end(),
                    {"TITLE=Scan Title",
                     "PERFORMER=Ada (Piano)",
                     "CONDUCTOR=First",
                     "SOLOIST=Anne",
                     "CONDUCTOR=Second",
                     "SOLOIST=Anne",
                     "PERFORMER=Ada (Piano)"});
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);
    writeScanFlacMetadataFixture(musicRoot / "song.flac", comments);
    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    auto const trackId = importOne(ml);
    auto const spec = storedTrackSpec(ml, trackId);

    CHECK(spec.credits == std::vector<library::Credit>{{.name = "First", .kind = library::CreditKind::Conductor},
                                                       {.name = "Second", .kind = library::CreditKind::Conductor},
                                                       expectedEnsembles[0],
                                                       expectedEnsembles[1],
                                                       {.name = "Anne", .kind = library::CreditKind::Soloist},
                                                       {.name = "Anne", .kind = library::CreditKind::Soloist},
                                                       {.name = "Ada", .role = "Piano"},
                                                       {.name = "Ada", .role = "Piano"}});
    CHECK_FALSE(spec.recordingDate.isPresent());
  }

  TEST_CASE("ScanApplyOperation - malformed new credit or artist text fails only that item at serialization",
            "[runtime][integration][library-scan]")
  {
    auto invalidComment = std::string{};

    SECTION("malformed credit name")
    {
      invalidComment = "CONDUCTOR=bad\xFFname";
    }

    SECTION("malformed credit role")
    {
      invalidComment = "PERFORMER=Rejected performer (bad\xFFrole)";
    }

    SECTION("malformed Artist control")
    {
      invalidComment = "ARTIST=bad\xFFname";
    }

    auto const temp = ao::test::TempDir{};
    auto const musicRoot = temp.path() / "music";
    std::filesystem::create_directories(musicRoot);
    writeScanFlacMetadataFixture(
      musicRoot / "bad.flac",
      {"TITLE=Rejected title", "ALBUM=Rejected album", "CONDUCTOR=Rejected conductor", invalidComment});
    writeScanFlacMetadataFixture(musicRoot / "good.flac", {"TITLE=Good", "CONDUCTOR=Valid", "PERFORMER=Alice (Piano)"});
    auto ml = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");
    auto const revisionBefore = [&]
    {
      auto transaction = ml.readTransaction();
      return ml.libraryRevision(transaction);
    }();
    auto plan = LibraryScan{ml}.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::New) == 2);
    auto failureUris = std::vector<std::string>{};
    auto failureStages = std::vector<std::string>{};
    auto failureMessages = std::vector<std::string>{};
    auto operation = ScanApplyOperation{ml,
                                        std::move(plan),
                                        {},
                                        [&](ScanFailure const& failure)
                                        {
                                          failureUris.emplace_back(failure.uri);
                                          failureStages.emplace_back(failure.stage);
                                          failureMessages.emplace_back(failure.message);
                                        }};

    auto const runRes = operation.run();

    REQUIRE(runRes);
    REQUIRE(runRes->insertedIds.size() == 1);
    CHECK(runRes->mutatedIds.empty());
    CHECK(runRes->relinkedIds.empty());
    CHECK(runRes->missingCount == 0);
    CHECK(runRes->staleCount == 0);
    CHECK(runRes->failureCount == 1);
    CHECK(runRes->libraryRevision == revisionBefore + 1);
    CHECK(failureUris == std::vector<std::string>{"bad.flac"});
    CHECK(failureStages == std::vector<std::string>{"serialize"});
    REQUIRE(failureMessages.size() == 1);
    CHECK_FALSE(failureMessages.front().empty());
    auto const spec = storedTrackSpec(ml, runRes->insertedIds.front());
    CHECK(spec.uri == "good.flac");
    CHECK(spec.title == "Good");
    CHECK(spec.artist.empty());
    CHECK(spec.album.empty());
    CHECK(spec.duration == std::chrono::seconds{1});
    CHECK_FALSE(spec.recordingDate.isPresent());
    CHECK(spec.credits == std::vector<library::Credit>{{.name = "Valid", .kind = library::CreditKind::Conductor},
                                                       {.name = "Alice", .role = "Piano"}});

    auto transaction = ml.readTransaction();
    CHECK(ml.libraryRevision(transaction) == runRes->libraryRevision);
    CHECK(ml.tracks().reader(transaction).entryCount() == 1);
    auto manifest = ml.manifest().reader(transaction);
    CHECK_FALSE(manifest.get("bad.flac"));
    auto const optGood = manifest.get("good.flac");
    REQUIRE(optGood);
    CHECK(optGood->trackId() == runRes->insertedIds.front());
    CHECK(optGood->status() == library::FileStatus::Available);
    CHECK(optGood->fileSize() == std::filesystem::file_size(musicRoot / "good.flac"));
    auto const timestampRes = library::lastWriteTimestamp(musicRoot / "good.flac");
    REQUIRE(timestampRes);
    REQUIRE(optGood->mtime());
    CHECK(*optGood->mtime() == *timestampRes);
    CHECK(optGood->audioPayloadLength() == 2);
    CHECK(optGood->audioSignature() != utility::Hash128{});
    CHECK(ml.dictionary().size() == 3);
    CHECK(ml.dictionary().findId("Valid").has_value());
    CHECK(ml.dictionary().findId("Alice").has_value());
    CHECK(ml.dictionary().findId("Piano").has_value());
    CHECK_FALSE(ml.dictionary().findId("Rejected title"));
    CHECK_FALSE(ml.dictionary().findId("Rejected album"));
    CHECK_FALSE(ml.dictionary().findId("Rejected conductor"));
    CHECK_FALSE(ml.dictionary().findId("Rejected performer"));
    CHECK_FALSE(ml.dictionary().findId("bad\xFFname"));
    CHECK_FALSE(ml.dictionary().findId("bad\xFFrole"));
    CHECK_FALSE(ml.dictionary().findId("bad\xEF\xBF\xBDname"));
    CHECK_FALSE(ml.dictionary().findId("bad\xEF\xBF\xBDrole"));
  }

  TEST_CASE(
    "ScanApplyOperation - malformed rescan metadata preserves curation and commits refreshed facts with new siblings",
    "[runtime][integration][library-scan]")
  {
    auto const invalidComments = std::to_array<std::string>(
      {"CONDUCTOR=bad\xFFname", "PERFORMER=Rejected performer (bad\xFFrole)", "ARTIST=bad\xFFname"});

    for (auto const& invalidComment : invalidComments)
    {
      for (bool const moved : {false, true})
      {
        CAPTURE(invalidComment, moved);
        auto const temp = ao::test::TempDir{};
        auto const musicRoot = temp.path() / "music";
        std::filesystem::create_directories(musicRoot);
        auto const originalFile = musicRoot / "song.flac";
        writeScanFlacMetadataFixture(originalFile, {"TITLE=Original", "PERFORMER=Imported (Piano)"});
        auto ml = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");
        auto const trackId = importOne(ml);
        auto const curatedCredits =
          std::vector<library::Credit>{{.name = "Curated", .kind = library::CreditKind::Conductor, .role = "Leader"},
                                       {.name = "Solo", .kind = library::CreditKind::Soloist},
                                       {.name = "Ada", .role = "Piano"},
                                       {.name = "Ada", .role = "Piano"}};
        updateTrackSpec(ml,
                        trackId,
                        [&](TrackSpec& spec)
                        {
                          spec.title = "Curated title";
                          spec.artist = "Curated artist";
                          spec.year = 1972;
                          spec.recordingDate = {.year = 1955, .month = 3};
                          spec.credits = curatedCredits;
                          spec.tags = {"favorite"};
                          spec.customMetadata = {{"Note", "Keep me"}};
                        });
        auto const revisionBefore = [&]
        {
          auto transaction = ml.readTransaction();
          return ml.libraryRevision(transaction);
        }();
        auto const retaggedFile = temp.path() / "retagged.flac";
        writeScanFlacMetadataFixture(
          retaggedFile, {"TITLE=Rejected title", "DATE=1999", "CONDUCTOR=Rejected conductor", invalidComment}, 88200);
        auto const destination = moved ? musicRoot / "renamed.flac" : originalFile;

        if (moved)
        {
          std::filesystem::copy_file(retaggedFile, destination);
          REQUIRE(std::filesystem::remove(originalFile));
        }
        else
        {
          replaceFile(originalFile, retaggedFile);
        }

        auto const sibling = musicRoot / "new.flac";
        writeScanFlacMetadataFixture(sibling, {"TITLE=New sibling", "CONDUCTOR=New conductor"});
        // A distinct payload prevents ambiguous move matching with this new sibling.
        {
          auto output = std::ofstream{sibling, std::ios::binary | std::ios::app};
          output.put('\x42');
          REQUIRE(output.good());
        }

        auto plan = LibraryScan{ml}.buildPlan().value();
        REQUIRE(plan.count(ScanClassification::New) == 1);
        REQUIRE(plan.count(ScanClassification::Changed) == (moved ? 0 : 1));
        REQUIRE(plan.count(ScanClassification::Moved) == (moved ? 1 : 0));
        auto failures = std::vector<std::string>{};
        auto operation = ScanApplyOperation{
          ml, std::move(plan), {}, [&](ScanFailure const& failure) { failures.emplace_back(failure.message); }};

        auto const runRes = operation.run();

        REQUIRE(runRes);
        REQUIRE(runRes->insertedIds.size() == 1);
        CHECK(runRes->insertedIds.front() != trackId);
        CHECK(runRes->mutatedIds == (moved ? std::vector<TrackId>{} : std::vector{trackId}));
        CHECK(runRes->relinkedIds == (moved ? std::vector{trackId} : std::vector<TrackId>{}));
        CHECK(runRes->failureCount == 0);
        CHECK(runRes->staleCount == 0);
        CHECK(runRes->missingCount == 0);
        CHECK(runRes->libraryRevision == revisionBefore + 1);
        CHECK(failures.empty());
        auto const spec = storedTrackSpec(ml, trackId);
        CHECK(spec.uri == (moved ? "renamed.flac" : "song.flac"));
        CHECK(spec.title == "Curated title");
        CHECK(spec.artist == "Curated artist");
        CHECK(spec.year == 1972);
        CHECK(spec.recordingDate == library::RecordingDate{.year = 1955, .month = 3});
        CHECK(spec.credits == curatedCredits);
        CHECK(spec.tags == std::vector<std::string>{"favorite"});
        CHECK(spec.customMetadata == std::vector<std::pair<std::string, std::string>>{{"Note", "Keep me"}});
        CHECK(spec.duration == std::chrono::seconds{2});
        auto const fileRes = readMediaTrack(destination);
        REQUIRE(fileRes);
        CHECK(spec.bitrate == fileRes->builder().property().bitrate());
        CHECK(spec.sampleRate == fileRes->builder().property().sampleRate());
        auto const newSpec = storedTrackSpec(ml, runRes->insertedIds.front());
        CHECK(newSpec.uri == "new.flac");
        CHECK(newSpec.title == "New sibling");
        CHECK(newSpec.credits ==
              std::vector<library::Credit>{{.name = "New conductor", .kind = library::CreditKind::Conductor}});
        auto transaction = ml.readTransaction();
        CHECK(ml.libraryRevision(transaction) == runRes->libraryRevision);
        CHECK(ml.tracks().reader(transaction).entryCount() == 2);
        auto manifest = ml.manifest().reader(transaction);
        auto const optRefreshed = manifest.get(spec.uri);
        REQUIRE(optRefreshed);
        CHECK(optRefreshed->trackId() == trackId);
        CHECK(optRefreshed->status() == library::FileStatus::Available);
        CHECK(optRefreshed->fileSize() == std::filesystem::file_size(destination));
        auto const timestampRes = library::lastWriteTimestamp(destination);
        REQUIRE(timestampRes);
        REQUIRE(optRefreshed->mtime());
        CHECK(*optRefreshed->mtime() == *timestampRes);
        CHECK(optRefreshed->audioPayloadLength() == 2);
        CHECK(optRefreshed->audioSignature() != utility::Hash128{});
        auto const optNew = manifest.get("new.flac");
        REQUIRE(optNew);
        CHECK(optNew->trackId() == runRes->insertedIds.front());
        CHECK(optNew->status() == library::FileStatus::Available);
        CHECK(optNew->fileSize() == std::filesystem::file_size(sibling));
        CHECK(optNew->audioPayloadLength() == 3);
        CHECK(optNew->audioSignature() != utility::Hash128{});

        if (moved)
        {
          CHECK_FALSE(manifest.get("song.flac"));
        }

        CHECK_FALSE(ml.dictionary().findId("Rejected title"));
        CHECK_FALSE(ml.dictionary().findId("Rejected conductor"));
        CHECK_FALSE(ml.dictionary().findId("Rejected performer"));
        CHECK_FALSE(ml.dictionary().findId("bad\xFFname"));
        CHECK_FALSE(ml.dictionary().findId("bad\xFFrole"));
      }
    }
  }

  TEST_CASE("ScanApplyOperation - changed-file scans keep stored credits and recording date",
            "[runtime][integration][library-scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);
    auto const scannedFile = musicRoot / "song.flac";
    writeScanFlacMetadataFixture(scannedFile, {"TITLE=Original Title", "PERFORMER=Ada (Piano)"});
    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    auto const trackId = importOne(ml);
    REQUIRE(storedTrackSpec(ml, trackId).credits == std::vector<library::Credit>{{.name = "Ada", .role = "Piano"}});

    updateTrackSpec(ml,
                    trackId,
                    [](TrackSpec& spec)
                    {
                      spec.credits = {{.name = "Gould", .role = "Piano"}};
                      spec.recordingDate = {.year = 1981, .month = 5, .day = 12};
                    });
    auto const retaggedFile = std::filesystem::path{temp.path()} / "retagged.flac";
    writeScanFlacMetadataFixture(retaggedFile, {"TITLE=Retagged Title", "PERFORMER=Bob (Violin)"}, 88200);
    replaceFile(scannedFile, retaggedFile);
    auto plan = LibraryScan{ml}.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::Changed) == 1);
    auto operation = ScanApplyOperation{ml, std::move(plan), {}, {}};

    auto const runRes = operation.run();

    REQUIRE(runRes);
    CHECK(runRes->failureCount == 0);
    auto const changedSpec = storedTrackSpec(ml, trackId);
    CHECK(changedSpec.title == "Original Title");
    CHECK(changedSpec.duration == std::chrono::seconds{2});
    CHECK(changedSpec.credits == std::vector<library::Credit>{{.name = "Gould", .role = "Piano"}});
    CHECK(changedSpec.recordingDate == library::RecordingDate{.year = 1981, .month = 5, .day = 12});
  }

  TEST_CASE("ScanApplyOperation - changed-file scans do not populate absent credits",
            "[runtime][integration][library-scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);
    auto const scannedFile = musicRoot / "song.flac";
    writeScanFlacMetadataFixture(scannedFile, {"TITLE=Original Title"});
    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    auto const trackId = importOne(ml);
    CHECK(storedTrackSpec(ml, trackId).credits.empty());
    auto const retaggedFile = std::filesystem::path{temp.path()} / "retagged.flac";
    writeScanFlacMetadataFixture(
      retaggedFile, {"TITLE=Retagged Title", "PERFORMER=Ada (Piano)", "CONDUCTOR=New"}, 88200);
    replaceFile(scannedFile, retaggedFile);
    auto plan = LibraryScan{ml}.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::Changed) == 1);
    auto operation = ScanApplyOperation{ml, std::move(plan), {}, {}};

    auto const runRes = operation.run();

    REQUIRE(runRes);
    CHECK(runRes->failureCount == 0);
    // An ordinary rescan refreshes technical properties only.
    auto const changedSpec = storedTrackSpec(ml, trackId);
    CHECK(changedSpec.title == "Original Title");
    CHECK(changedSpec.duration == std::chrono::seconds{2});
    CHECK(changedSpec.credits.empty());
    CHECK_FALSE(changedSpec.recordingDate.isPresent());
  }

  TEST_CASE("ScanApplyOperation - changed and moved rescans keep curated year apart from the file year",
            "[runtime][integration][library-scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);
    auto const scannedFile = musicRoot / "song.flac";
    writeScanFlacMetadataFixture(
      scannedFile, {"TITLE=Original Title", "DATE=1981", "SOLOIST=Imported Soloist", "PERFORMER=Ada (Piano)"});
    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    auto const trackId = importOne(ml);
    auto const imported = storedTrackSpec(ml, trackId);
    CHECK(imported.year == 1981);
    CHECK_FALSE(imported.recordingDate.isPresent());
    REQUIRE(imported.credits ==
            std::vector<library::Credit>{
              {.name = "Imported Soloist", .kind = library::CreditKind::Soloist}, {.name = "Ada", .role = "Piano"}});
    auto const curatedDate = library::RecordingDate{.year = 1955, .month = 3, .day = 0};
    auto const curatedCredits = std::vector<library::Credit>{
      {.name = "Curated Conductor", .kind = library::CreditKind::Conductor},
      {.name = "Curated Ensemble", .kind = library::CreditKind::Ensemble},
      {.name = "Curated Soloist", .kind = library::CreditKind::Soloist},
      {.name = "Gould", .role = "Piano"},
      {.name = "Gould", .role = "Piano"},
      {.name = "Ada"},
    };
    updateTrackSpec(ml,
                    trackId,
                    [&](TrackSpec& spec)
                    {
                      spec.year = 1972;
                      spec.recordingDate = curatedDate;
                      spec.credits = curatedCredits;
                    });
    auto const retagComments = std::vector<std::string>{
      "TITLE=Retagged Title", "DATE=1999", "SOLOIST=File Soloist", "PERFORMER=Bob (Violin)", "PERFORMER=Cara"};
    auto const expectCurated = [&](std::filesystem::path const& filePath, std::string_view expectedUri)
    {
      auto const spec = storedTrackSpec(ml, trackId);
      CHECK(spec.uri == expectedUri);
      CHECK(spec.title == "Original Title");
      CHECK(spec.year == 1972);
      CHECK(spec.year != curatedDate.year);
      CHECK(spec.recordingDate == curatedDate);
      CHECK(spec.credits == curatedCredits);
      CHECK(spec.duration == std::chrono::seconds{2});
      auto const fileTrackRes = readMediaTrack(filePath);
      REQUIRE(fileTrackRes);
      auto const& fileMetadata = fileTrackRes->builder().metadata();
      CHECK(fileMetadata.year() == 1999);
      CHECK(fileMetadata.year() != spec.year);
      CHECK_FALSE(fileMetadata.recordingDate().isPresent());
      REQUIRE(fileMetadata.credits().size() == 3);
      CHECK(fileMetadata.credits()[0].name == "File Soloist");
      CHECK(fileMetadata.credits()[0].kind == library::CreditKind::Soloist);
      CHECK(fileMetadata.credits()[0].role.empty());
      CHECK(fileMetadata.credits()[1].name == "Bob");
      CHECK(fileMetadata.credits()[1].kind == library::CreditKind::Performer);
      CHECK(fileMetadata.credits()[1].role == "Violin");
      CHECK(fileMetadata.credits()[2].name == "Cara");
      CHECK(fileMetadata.credits()[2].kind == library::CreditKind::Performer);
      CHECK(fileMetadata.credits()[2].role.empty());
    };

    SECTION("a changed file keeps the curated values")
    {
      auto const retaggedFile = std::filesystem::path{temp.path()} / "retagged.flac";
      writeScanFlacMetadataFixture(retaggedFile, retagComments, 88200);
      replaceFile(scannedFile, retaggedFile);
      auto plan = LibraryScan{ml}.buildPlan().value();
      REQUIRE(plan.count(ScanClassification::Changed) == 1);
      REQUIRE(plan.count(ScanClassification::Moved) == 0);
      auto operation = ScanApplyOperation{ml, std::move(plan), {}, {}};
      auto const runRes = operation.run();
      REQUIRE(runRes);
      CHECK(runRes->failureCount == 0);
      CHECK(runRes->mutatedIds == std::vector{trackId});
      CHECK(runRes->relinkedIds.empty());
      CHECK(runRes->insertedIds.empty());
      expectCurated(scannedFile, "song.flac");
    }

    SECTION("a moved file keeps the curated values")
    {
      auto const movedFile = musicRoot / "renamed.flac";
      writeScanFlacMetadataFixture(movedFile, retagComments, 88200);
      REQUIRE(std::filesystem::remove(scannedFile));
      auto plan = LibraryScan{ml}.buildPlan().value();
      REQUIRE(plan.count(ScanClassification::Moved) == 1);
      REQUIRE(plan.count(ScanClassification::Changed) == 0);
      REQUIRE(plan.count(ScanClassification::New) == 0);
      auto operation = ScanApplyOperation{ml, std::move(plan), {}, {}};
      auto const runRes = operation.run();
      REQUIRE(runRes);
      CHECK(runRes->failureCount == 0);
      CHECK(runRes->relinkedIds == std::vector{trackId});
      CHECK(runRes->mutatedIds.empty());
      CHECK(runRes->insertedIds.empty());
      expectCurated(movedFile, "renamed.flac");
    }
  }
} // namespace ao::rt::test
