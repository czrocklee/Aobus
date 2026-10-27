// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/ScanApplyOperation.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/library/WritableLibraryTestSupport.h"
#include "test/media/ogg/TestOgg.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include "test/runtime/library/ScanApplyTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWriter.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/ScanPlan.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
    constexpr auto kSheet = "TITLE \"Disc\"\nPERFORMER \"Orchestra\"\nFILE \"rip.wav\" WAVE\n"
                            "TRACK 01 AUDIO\nTITLE \"First\"\nINDEX 01 00:00:00\n"
                            "TRACK 03 AUDIO\nTITLE \"Third\"\nINDEX 01 00:01:00\n";

    void writeSheet(std::filesystem::path const& path, std::string_view text = kSheet)
    {
      auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
      output << text;
      REQUIRE(output.good());
    }

    std::filesystem::path makeMusicRoot(std::filesystem::path const& root)
    {
      auto path = root / "music";
      std::filesystem::create_directories(path);
      return path;
    }

    struct CueScanFixture final
    {
      ao::test::TempDir temp;
      std::filesystem::path musicRoot = makeMusicRoot(temp.path());
      library::MusicLibrary library = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");

      CueScanFixture()
      {
        writeScanFlacMetadataFixture(musicRoot / "image.flac",
                                     {"TITLE=Carrier",
                                      "ARTIST=Carrier artist",
                                      "ALBUM=Carrier album",
                                      "ALBUMARTIST=Carrier album artist",
                                      "COMPOSER=Carrier composer",
                                      "GENRE=Classical",
                                      "DATE=1999",
                                      "DISCNUMBER=2",
                                      "DISCTOTAL=4",
                                      "WORK=Carrier work"},
                                     88200);
        writeSheet(musicRoot / "image.cue");
      }

      ScanPlan plan() const
      {
        auto res = LibraryScan{library}.buildPlan();
        REQUIRE(res);
        return std::move(*res);
      }

      ScanApplyResult scan(ScanApplyOptions options = {})
      {
        auto res = ScanApplyOperation{library, plan(), {}, {}, options}.run();
        REQUIRE(res);
        return std::move(*res);
      }

      void remove(TrackId id)
      {
        auto transaction = library::test::writeTransaction(library);
        REQUIRE(transaction.apply(
          [id](library::LibraryWrite& write) -> Result<>
          {
            auto res = write.tracks().remove(id);
            REQUIRE(res);
            REQUIRE(*res);
            return {};
          }));
        REQUIRE(transaction.commit());
      }
    };
  } // namespace

  TEST_CASE("CUE scan - new carrier imports sparse ordinals with exact ranges and metadata fallbacks",
            "[runtime][integration][cue-scan]")
  {
    auto fixture = CueScanFixture{};
    auto const res = fixture.scan();
    REQUIRE(res.insertedIds.size() == 2);
    CHECK(res.failureCount == 0);
    auto transaction = fixture.library.readTransaction();
    auto optManifest = fixture.library.manifest().reader(transaction).get("image.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->segmentCount() == 2);
    CHECK(optManifest->totalFrames() == 88200);
    CHECK(optManifest->trackId() == kInvalidTrackId);
    auto optFirst = fixture.library.tracks().reader(transaction).get(res.insertedIds[0]);
    auto optThird = fixture.library.tracks().reader(transaction).get(res.insertedIds[1]);
    REQUIRE(optFirst);
    REQUIRE(optThird);
    REQUIRE(optFirst->property().segment());
    REQUIRE(optThird->property().segment());
    CHECK(optFirst->property().segment()->ordinal == 1);
    CHECK(optFirst->property().segment()->startFrame == 0);
    CHECK(optFirst->property().segment()->frameCount == 44100);
    CHECK(optThird->property().segment()->ordinal == 3);
    CHECK(optThird->property().segment()->startFrame == 44100);
    CHECK(optThird->property().duration() == std::chrono::milliseconds{1000});
    auto const spec = library::test::trackSpecFromView(fixture.library, *optThird);
    CHECK(spec.title == "Third");
    CHECK(spec.artist == "Orchestra");
    CHECK(spec.album == "Disc");
    CHECK(spec.albumArtist == "Orchestra");
    CHECK(spec.composer == "Carrier composer");
    CHECK(spec.genre == "Classical");
    CHECK(spec.year == 1999);
    CHECK(spec.discNumber == 2);
    CHECK(spec.discTotal == 4);
    CHECK(spec.work == "Carrier work");
    CHECK(spec.trackNumber == 3);
    CHECK(spec.trackTotal == 2);
  }

  TEST_CASE("CUE scan - byte edits with unchanged sheet size and timestamp stale prepared imports",
            "[runtime][integration][cue-scan][concurrency]")
  {
    auto fixture = CueScanFixture{};
    auto operation = ScanApplyOperation{fixture.library, fixture.plan(), {}, {}};
    requirePrepared(operation);
    auto const path = fixture.musicRoot / "image.cue";
    auto const time = std::filesystem::last_write_time(path);
    auto const size = std::filesystem::file_size(path);
    auto sheet = std::string{kSheet};
    sheet.replace(sheet.find("00:01:00"), 8, "00:00:30");
    writeSheet(path, sheet);
    std::filesystem::last_write_time(path, time);
    REQUIRE(std::filesystem::file_size(path) == size);
    requireRevalidation(operation, 1, 0);
    auto res = operation.run();
    REQUIRE(res);
    CHECK(res->insertedIds.empty());
    auto transaction = fixture.library.readTransaction();
    CHECK_FALSE(fixture.library.manifest().reader(transaction).get("image.flac"));
  }

  TEST_CASE("CUE scan - case variant sheet insertion changes candidate sets before apply",
            "[runtime][integration][cue-scan][concurrency]")
  {
    auto fixture = CueScanFixture{};
    auto operation = ScanApplyOperation{fixture.library, fixture.plan(), {}, {}};
    requirePrepared(operation);
    writeSheet(fixture.musicRoot / "image.FLAC.CUE");
    auto revalidatedRes = operation.revalidatePreparedFiles();
    REQUIRE(revalidatedRes);
    CHECK(revalidatedRes->staleCount == 1);
    auto res = operation.run();
    REQUIRE(res);
    CHECK(res->insertedIds.empty());
    auto transaction = fixture.library.readTransaction();
    CHECK_FALSE(fixture.library.manifest().reader(transaction).get("image.flac"));
  }

  TEST_CASE("CUE scan - missing ordinal is actionable and appended without rewriting surviving members",
            "[runtime][integration][cue-scan]")
  {
    auto fixture = CueScanFixture{};
    auto const imported = fixture.scan();
    REQUIRE(imported.insertedIds.size() == 2);
    auto const survivor = imported.insertedIds[0];
    fixture.remove(imported.insertedIds[1]);
    library::test::mutateTrack(
      fixture.library, survivor, [](library::TrackBuilder& builder) { builder.metadata().title("Curated"); });
    auto plan = fixture.plan();
    CHECK(plan.count(ScanClassification::MissingSegments) == 1);
    CHECK(plan.count(ScanClassification::New) == 0);
    auto res = ScanApplyOperation{fixture.library, std::move(plan), {}, {}}.run();
    REQUIRE(res);
    REQUIRE(res->insertedIds.size() == 1);
    CHECK(res->mutatedIds.empty());
    auto transaction = fixture.library.readTransaction();
    auto optManifest = fixture.library.manifest().reader(transaction).get("image.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->segmentCount() == 2);
    CHECK(optManifest->segmentTrackIds()[0] == survivor);
    CHECK(optManifest->segmentTrackIds()[1] == res->insertedIds[0]);
    auto optTrack = fixture.library.tracks().reader(transaction).get(survivor);
    REQUIRE(optTrack);
    CHECK(optTrack->metadata().title() == "Curated");
    CHECK(optTrack->property().segment()->frameCount == 44100);
  }

  TEST_CASE("CUE scan - deleting a carrier member after preparation makes its structural evidence stale",
            "[runtime][integration][cue-scan][concurrency]")
  {
    auto fixture = CueScanFixture{};
    auto const imported = fixture.scan();
    REQUIRE(imported.insertedIds.size() == 2);
    fixture.remove(imported.insertedIds[1]);
    auto operation = ScanApplyOperation{fixture.library, fixture.plan(), {}, {}};
    requirePrepared(operation);
    fixture.remove(imported.insertedIds[0]);
    requireRevalidation(operation, 0, 0);
    auto res = operation.run();
    REQUIRE(res);
    CHECK(res->staleCount == 1);
    CHECK(res->failureCount == 0);
    CHECK(res->insertedIds.empty());
    auto transaction = fixture.library.readTransaction();
    CHECK_FALSE(fixture.library.manifest().reader(transaction).get("image.flac"));
  }

  TEST_CASE("CUE scan - moving a carrier counts one physical file for all member Tracks",
            "[runtime][integration][cue-scan]")
  {
    auto fixture = CueScanFixture{};
    auto const imported = fixture.scan();
    REQUIRE(imported.insertedIds.size() == 2);
    CHECK(imported.relinkedFileCount == 0);
    std::filesystem::rename(fixture.musicRoot / "image.flac", fixture.musicRoot / "renamed.flac");
    std::filesystem::rename(fixture.musicRoot / "image.cue", fixture.musicRoot / "renamed.cue");
    auto plan = fixture.plan();
    CHECK(plan.count(ScanClassification::Moved) == 1);
    CHECK(plan.count(ScanClassification::Error) == 0);
    auto res = ScanApplyOperation{fixture.library, std::move(plan), {}, {}}.run();
    REQUIRE(res);
    CHECK(res->relinkedIds == imported.insertedIds);
    CHECK(res->relinkedFileCount == 1);
    CHECK(res->failureCount == 0);
  }

  TEST_CASE("CUE scan - moving a carrier without its sheet preserves every member identity and range",
            "[runtime][integration][cue-scan]")
  {
    auto fixture = CueScanFixture{};
    auto const imported = fixture.scan();
    REQUIRE(imported.insertedIds.size() == 2);
    std::filesystem::rename(fixture.musicRoot / "image.flac", fixture.musicRoot / "renamed.flac");
    auto plan = fixture.plan();
    CHECK(plan.count(ScanClassification::Moved) == 1);
    CHECK(plan.count(ScanClassification::Error) == 0);
    CHECK(plan.count(ScanClassification::Notice) == 1);
    auto const moved = std::ranges::find(plan.items(), ScanClassification::Moved, &ScanItem::classification);
    REQUIRE(moved != plan.items().end());
    CHECK_FALSE(moved->errorMessage.empty());
    CHECK(moved->cueDiagnostic == ScanCueDiagnostic::SheetMissingForCarrier);
    CHECK(isScanNotice(*moved));
    auto res = ScanApplyOperation{fixture.library, std::move(plan), {}, {}}.run();
    REQUIRE(res);
    CHECK(res->relinkedIds == imported.insertedIds);
    CHECK(res->relinkedFileCount == 1);
    CHECK(res->insertedIds.empty());
    CHECK(res->failureCount == 0);
    auto transaction = fixture.library.readTransaction();
    CHECK_FALSE(fixture.library.manifest().reader(transaction).get("image.flac"));
    auto optManifest = fixture.library.manifest().reader(transaction).get("renamed.flac");
    REQUIRE(optManifest);
    auto const memberIds =
      std::vector<TrackId>{optManifest->segmentTrackIds().begin(), optManifest->segmentTrackIds().end()};
    CHECK(memberIds == imported.insertedIds);

    for (auto const id : imported.insertedIds)
    {
      auto optTrack = fixture.library.tracks().reader(transaction).get(id);
      REQUIRE(optTrack);
      CHECK(optTrack->property().uri() == "renamed.flac");
      REQUIRE(optTrack->property().segment());
      CHECK(optTrack->property().segment()->frameCount == 44100);
    }
  }

  TEST_CASE("CUE scan - changed carrier with another exact extent is rejected without modifying members",
            "[runtime][integration][cue-scan]")
  {
    auto fixture = CueScanFixture{};
    auto const imported = fixture.scan();
    REQUIRE(imported.insertedIds.size() == 2);
    auto const path = fixture.musicRoot / "image.flac";
    auto const time = std::filesystem::last_write_time(path);
    writeScanFlacMetadataFixture(path, {"TITLE=Replacement"}, 132300);
    std::filesystem::last_write_time(path, time + std::chrono::seconds{2});
    auto plan = fixture.plan();
    CHECK(plan.count(ScanClassification::Error) == 1);
    CHECK(plan.items()[0].cueDiagnostic == ScanCueDiagnostic::CarrierFactsMismatch);
    auto res = ScanApplyOperation{fixture.library, std::move(plan), {}, {}}.run();
    REQUIRE(res);
    CHECK(res->mutatedIds.empty());
    CHECK(res->failureCount == 1);
    auto transaction = fixture.library.readTransaction();
    auto optManifest = fixture.library.manifest().reader(transaction).get("image.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->totalFrames() == 88200);
    auto optTrack = fixture.library.tracks().reader(transaction).get(imported.insertedIds[1]);
    REQUIRE(optTrack);
    CHECK(optTrack->metadata().title() == "Third");
    CHECK(optTrack->property().segment()->frameCount == 44100);
  }

  TEST_CASE("CUE scan - changed carrier refreshes all members while preserving concurrently curated metadata",
            "[runtime][integration][cue-scan][concurrency]")
  {
    auto fixture = CueScanFixture{};
    auto const imported = fixture.scan();
    REQUIRE(imported.insertedIds.size() == 2);
    auto const path = fixture.musicRoot / "image.flac";
    auto const time = std::filesystem::last_write_time(path);
    writeScanFlacMetadataFixture(path, {"TITLE=Retagged", "ARTIST=Retagged artist"}, 88200);
    std::filesystem::last_write_time(path, time + std::chrono::seconds{2});
    std::filesystem::remove(fixture.musicRoot / "image.cue");
    auto plan = fixture.plan();
    CHECK(plan.count(ScanClassification::Changed) == 1);
    auto operation = ScanApplyOperation{fixture.library, std::move(plan), {}, {}};
    REQUIRE(operation.prepare());
    library::test::mutateTrack(fixture.library,
                               imported.insertedIds[0],
                               [](library::TrackBuilder& builder)
                               { builder.metadata().title("Curated during preparation"); });
    auto res = operation.run();
    REQUIRE(res);
    CHECK(res->mutatedIds == imported.insertedIds);
    CHECK(res->failureCount == 0);
    auto transaction = fixture.library.readTransaction();
    auto optFirst = fixture.library.tracks().reader(transaction).get(imported.insertedIds[0]);
    auto optThird = fixture.library.tracks().reader(transaction).get(imported.insertedIds[1]);
    REQUIRE(optFirst);
    REQUIRE(optThird);
    CHECK(optFirst->metadata().title() == "Curated during preparation");
    CHECK(optThird->metadata().title() == "Third");
    CHECK(optFirst->property().duration() == std::chrono::milliseconds{1000});
    CHECK(optThird->property().sampleRate() == SampleRate{44100});
    auto optManifest = fixture.library.manifest().reader(transaction).get("image.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->fileSize() == std::filesystem::file_size(path));
  }

  TEST_CASE("CUE scan - empty sheet values fall back like absent ones", "[runtime][integration][cue-scan]")
  {
    auto fixture = CueScanFixture{};
    writeSheet(fixture.musicRoot / "image.cue",
               "TITLE \"\"\nPERFORMER \"\"\nFILE \"rip.wav\" WAVE\n"
               "TRACK 01 AUDIO\nTITLE \"\"\nPERFORMER \"Soloist\"\nINDEX 01 00:00:00\n"
               "TRACK 03 AUDIO\nTITLE \"Third\"\nPERFORMER \"\"\nINDEX 01 00:01:00\n");
    auto const res = fixture.scan();
    REQUIRE(res.insertedIds.size() == 2);
    auto const first = storedTrackSpec(fixture.library, res.insertedIds[0]);
    auto const third = storedTrackSpec(fixture.library, res.insertedIds[1]);
    // An empty value usually means unknown, so the next source applies.
    CHECK(first.title == "Carrier");
    CHECK(first.artist == "Soloist");
    CHECK(first.album == "Carrier album");
    CHECK(first.albumArtist == "Carrier album artist");
    CHECK(third.title == "Third");
    CHECK(third.artist == "Carrier artist");
  }

  TEST_CASE("CUE scan - an unmatched image sheet reports its name without importing unrelated files",
            "[runtime][integration][cue-scan]")
  {
    auto fixture = CueScanFixture{};
    std::filesystem::remove(fixture.musicRoot / "image.flac");
    auto plan = fixture.plan();
    REQUIRE(plan.size() == 1);
    CHECK(plan.items()[0].classification == ScanClassification::Notice);
    CHECK(plan.items()[0].uri == "image.cue");
    CHECK(plan.items()[0].cueDiagnostic == ScanCueDiagnostic::NoMatchingAudio);
    CHECK(plan.items()[0].errorMessage.contains("No supported audio file"));
    auto res = ScanApplyOperation{fixture.library, std::move(plan), {}, {}}.run();
    REQUIRE(res);
    CHECK(res->insertedIds.empty());
    CHECK(res->failureCount == 0);
  }

  TEST_CASE("CUE scan - embedded fallback imports segments and malformed external sheets block it",
            "[runtime][integration][cue-scan]")
  {
    auto fixture = CueScanFixture{};
    writeScanFlacMetadataFixture(fixture.musicRoot / "image.flac", {"CUESHEET=" + std::string{kSheet}}, 88200);

    SECTION("No external sheet uses embedded metadata")
    {
      std::filesystem::remove(fixture.musicRoot / "image.cue");
      auto res = fixture.scan();
      REQUIRE(res.insertedIds.size() == 2);
      CHECK(storedTrackSpec(fixture.library, res.insertedIds[1]).title == "Third");
    }

    SECTION("An external parse failure is not an embedded fallback")
    {
      writeSheet(fixture.musicRoot / "image.cue", "not a CUE sheet");
      auto res = fixture.scan();
      CHECK(res.insertedIds.empty());
      CHECK(res.failureCount == 1);
      auto transaction = fixture.library.readTransaction();
      CHECK_FALSE(fixture.library.manifest().reader(transaction).get("image.flac"));
    }
  }

  TEST_CASE("CUE scan - only different nonempty embedded CUESHEET comments compete", "[runtime][integration][cue-scan]")
  {
    auto fixture = CueScanFixture{};
    std::filesystem::remove(fixture.musicRoot / "image.cue");
    auto const sheetComment = "CUESHEET=" + std::string{kSheet};
    auto comments = std::vector<std::string>{};
    bool competes = false;

    SECTION("identical copies are one sheet")
    {
      comments = {sheetComment, sheetComment};
    }

    SECTION("an empty comment names no sheet")
    {
      comments = {"CUESHEET=", sheetComment};
    }

    SECTION("different sheets compete")
    {
      auto other = std::string{kSheet};
      other.replace(other.find("Third"), 5, "Other");
      comments = {sheetComment, "CUESHEET=" + other};
      competes = true;
    }

    writeScanFlacMetadataFixture(fixture.musicRoot / "image.flac", comments, 88200);
    auto plan = fixture.plan();
    REQUIRE(plan.size() == 1);

    if (auto const& item = plan.items().front(); competes)
    {
      // Choosing either comment would silently drop the other's layout.
      CHECK(item.classification == ScanClassification::Error);
      CHECK(item.cueDiagnostic == ScanCueDiagnostic::AmbiguousSheets);
      CHECK(item.cueDetail == "CUESHEET");
    }
    else
    {
      CHECK(item.classification == ScanClassification::New);
      CHECK(item.cueLayout.size() == 2);
    }

    auto res = ScanApplyOperation{fixture.library, std::move(plan), {}, {}}.run();
    REQUIRE(res);
    CHECK(res->insertedIds.size() == (competes ? 0 : 2));
    CHECK(res->failureCount == (competes ? 1 : 0));
  }

  TEST_CASE("CUE add file - creates and publishes every segment ID through the ordinary command",
            "[runtime][integration][cue-scan]")
  {
    auto fixture = CueScanFixture{};
    auto executor = QueuedExecutor{};
    auto changes = makeLibraryChanges(executor, fixture.library);
    auto commandsFixture = LibraryCommandsFixture{fixture.library, changes, executor};
    auto inserted = std::vector<TrackId>{};
    auto subscription =
      changes.onChanged([&inserted](LibraryChangeSet const& event) noexcept { inserted = event.tracksInserted; });
    auto res = commandsFixture.runTask(commandsFixture.commands().createTrackFromFileAsync("image.flac"));
    REQUIRE(res);
    REQUIRE(res->trackIds.size() == 2);
    CHECK(res->trackId == res->trackIds.front());
    CHECK(inserted == res->trackIds);
    auto transaction = fixture.library.readTransaction();
    auto optManifest = fixture.library.manifest().reader(transaction).get("image.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->segmentCount() == 2);
    CHECK(optManifest->totalFrames() == 88200);
    auto optThird = fixture.library.tracks().reader(transaction).get(res->trackIds[1]);
    REQUIRE(optThird);
    CHECK(optThird->metadata().title() == "Third");
    CHECK(optThird->property().segment()->ordinal == 3);
  }

  TEST_CASE("CUE add file - malformed external binding rejects creation without publication",
            "[runtime][integration][cue-scan]")
  {
    auto fixture = CueScanFixture{};
    writeSheet(fixture.musicRoot / "image.cue", "invalid sheet");
    auto executor = QueuedExecutor{};
    auto changes = makeLibraryChanges(executor, fixture.library);
    auto commandsFixture = LibraryCommandsFixture{fixture.library, changes, executor};
    std::size_t publicationCount = 0;
    auto subscription =
      changes.onChanged([&publicationCount](LibraryChangeSet const&) noexcept { ++publicationCount; });
    auto res = commandsFixture.runTask(commandsFixture.commands().createTrackFromFileAsync("image.flac"));
    REQUIRE_FALSE(res);
    CHECK(publicationCount == 0);
    auto transaction = fixture.library.readTransaction();
    CHECK_FALSE(fixture.library.manifest().reader(transaction).get("image.flac"));
    CHECK(fixture.library.tracks().reader(transaction).entryCount() == 0);
  }

  TEST_CASE("CUE scan - restoring a missing carrier without its sheet restores every member's physical row",
            "[runtime][integration][cue-scan]")
  {
    auto fixture = CueScanFixture{};
    auto const imported = fixture.scan();
    REQUIRE(imported.insertedIds.size() == 2);
    auto const path = fixture.musicRoot / "image.flac";
    auto const backup = fixture.temp.path() / "backup.flac";
    std::filesystem::rename(path, backup);
    auto missing = fixture.scan();
    CHECK(missing.missingCount == 1);
    std::filesystem::remove(fixture.musicRoot / "image.cue");
    std::filesystem::rename(backup, path);
    auto plan = fixture.plan();
    CHECK(plan.count(ScanClassification::Changed) == 1);
    auto res = ScanApplyOperation{fixture.library, std::move(plan), {}, {}}.run();
    REQUIRE(res);
    CHECK(res->mutatedIds == imported.insertedIds);
    CHECK(res->failureCount == 0);
    auto transaction = fixture.library.readTransaction();
    auto optManifest = fixture.library.manifest().reader(transaction).get("image.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->status() == library::FileStatus::Available);
    CHECK(optManifest->segmentCount() == 2);
    auto ids = std::vector<TrackId>{optManifest->segmentTrackIds().begin(), optManifest->segmentTrackIds().end()};
    CHECK(ids == imported.insertedIds);
  }

  TEST_CASE("CUE scan - text-only sheet edits preserve existing metadata and commit no changes",
            "[runtime][integration][cue-scan]")
  {
    auto fixture = CueScanFixture{};
    auto const imported = fixture.scan();
    REQUIRE(imported.insertedIds.size() == 2);
    auto sheet = std::string{kSheet};
    sheet.replace(sheet.find("Third"), 5, "Edited title");
    writeSheet(fixture.musicRoot / "image.cue", sheet);
    auto res = fixture.scan();
    CHECK(res.insertedIds.empty());
    CHECK(res.mutatedIds.empty());
    CHECK(res.failureCount == 0);
    CHECK(res.libraryRevision == 0);
    CHECK(storedTrackSpec(fixture.library, imported.insertedIds[1]).title == "Third");
  }

  TEST_CASE("CUE scan - a whole-file Track stays whole when a dividing sheet is added",
            "[runtime][integration][cue-scan]")
  {
    auto fixture = CueScanFixture{};
    std::filesystem::remove(fixture.musicRoot / "image.cue");
    auto const imported = fixture.scan();
    REQUIRE(imported.insertedIds.size() == 1);
    writeSheet(fixture.musicRoot / "image.cue");
    auto plan = fixture.plan();
    REQUIRE(plan.size() == 1);
    CHECK(plan.items()[0].classification == ScanClassification::Unchanged);
    CHECK(plan.items()[0].errorMessage.contains("whole file"));
    CHECK(plan.items()[0].cueDiagnostic == ScanCueDiagnostic::WholeFileAlreadyImported);
    auto res = ScanApplyOperation{fixture.library, std::move(plan), {}, {}}.run();
    REQUIRE(res);
    CHECK(res->insertedIds.empty());
    CHECK(res->mutatedIds.empty());
    CHECK(res->failureCount == 0);
    auto transaction = fixture.library.readTransaction();
    auto optTrack = fixture.library.tracks().reader(transaction).get(imported.insertedIds[0]);
    REQUIRE(optTrack);
    CHECK_FALSE(optTrack->property().segment());
    auto optManifest = fixture.library.manifest().reader(transaction).get("image.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->trackId() == imported.insertedIds[0]);
    CHECK(optManifest->segmentCount() == 0);
  }

  TEST_CASE("CUE scan - a layout mismatch names the first imported range the sheet no longer reproduces",
            "[runtime][integration][cue-scan]")
  {
    auto fixture = CueScanFixture{};
    REQUIRE(fixture.scan().insertedIds.size() == 2);
    auto expected = std::string{};

    SECTION("a moved index changes an imported range")
    {
      writeSheet(fixture.musicRoot / "image.cue",
                 "FILE \"rip.wav\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\nTRACK 03 AUDIO\nINDEX 01 00:01:30\n");
      expected = "TRACK 1 starts at frame 0 for 61740 frames, but was imported at frame 0 for 44100 frames";
    }

    SECTION("a renumbered track drops an imported range")
    {
      writeSheet(fixture.musicRoot / "image.cue",
                 "FILE \"rip.wav\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\nTRACK 02 AUDIO\nINDEX 01 00:01:00\n");
      expected = "it has no audio TRACK 3";
    }

    auto const plan = fixture.plan();
    REQUIRE(plan.size() == 1);
    CHECK(plan.items()[0].classification == ScanClassification::Unchanged);
    CHECK(plan.items()[0].cueDiagnostic == ScanCueDiagnostic::LayoutMismatch);
    CHECK(plan.items()[0].errorMessage.contains(expected));
    CHECK(plan.items()[0].errorMessage.contains("segments kept"));
  }

  TEST_CASE("CUE scan - explicit relink can resolve an identity ambiguity despite destination binding errors",
            "[runtime][integration][cue-scan]")
  {
    auto fixture = CueScanFixture{};
    auto const imported = fixture.scan();
    REQUIRE(imported.insertedIds.size() == 2);
    std::filesystem::copy_file(fixture.musicRoot / "image.flac", fixture.musicRoot / "duplicate.flac");
    auto const duplicate = fixture.scan();
    REQUIRE(duplicate.insertedIds.size() == 1);
    std::filesystem::remove(fixture.musicRoot / "duplicate.flac");
    std::filesystem::rename(fixture.musicRoot / "image.flac", fixture.musicRoot / "renamed.flac");
    writeSheet(fixture.musicRoot / "renamed.cue", "malformed sheet");
    auto plan = fixture.plan();
    CHECK(plan.count(ScanClassification::Moved) == 0);
    auto relinkRes = std::move(plan).makeRelinkPlan("image.flac", "renamed.flac");
    REQUIRE(relinkRes);
    auto res = ScanApplyOperation{fixture.library, std::move(*relinkRes), {}, {}}.run();
    REQUIRE(res);
    CHECK(res->relinkedIds == imported.insertedIds);
    CHECK(res->relinkedFileCount == 1);
    CHECK(res->failureCount == 0);
    auto transaction = fixture.library.readTransaction();
    CHECK_FALSE(fixture.library.manifest().reader(transaction).get("image.flac"));
    auto optManifest = fixture.library.manifest().reader(transaction).get("renamed.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->segmentCount() == 2);
    auto ids = std::vector<TrackId>{optManifest->segmentTrackIds().begin(), optManifest->segmentTrackIds().end()};
    CHECK(ids == imported.insertedIds);
  }

  TEST_CASE("CUE scan - invalid external encoding has a typed diagnostic and imports after repair",
            "[runtime][integration][cue-scan]")
  {
    auto fixture = CueScanFixture{};
    writeSheet(fixture.musicRoot / "image.cue", "TITLE \"\xff\"\n");
    auto plan = fixture.plan();
    REQUIRE(plan.size() == 1);
    CHECK(plan.items()[0].classification == ScanClassification::Error);
    CHECK(plan.items()[0].cueDiagnostic == ScanCueDiagnostic::InvalidEncoding);
    auto rejectedRes = ScanApplyOperation{fixture.library, std::move(plan), {}, {}}.run();
    REQUIRE(rejectedRes);
    CHECK(rejectedRes->insertedIds.empty());
    writeSheet(fixture.musicRoot / "image.cue");
    auto imported = fixture.scan();
    REQUIRE(imported.insertedIds.size() == 2);
    CHECK(storedTrackSpec(fixture.library, imported.insertedIds[0]).title == "First");
  }

  namespace
  {
    std::uint32_t littleEndian32(std::vector<std::uint8_t> const& bytes, std::size_t const offset)
    {
      REQUIRE(offset + 4 <= bytes.size());
      return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
             (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
             (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
    }

    std::int64_t littleEndian64(std::vector<std::uint8_t> const& bytes, std::size_t const offset)
    {
      REQUIRE(offset + 8 <= bytes.size());
      std::uint64_t value = 0;

      for (std::size_t index = 0; index < 8; ++index)
      {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
      }

      return static_cast<std::int64_t>(value);
    }

    // basic_metadata.opus keeps its OpusTags packet on one non-continued page.
    // Appending a comment and rewriting that page leaves the audio pages intact.
    void writeOpusFixture(std::filesystem::path const& destination, std::string_view const cueValue)
    {
      auto bytes = audio::test::readFileBytes(audio::test::requireAudioFixture("basic_metadata.opus"));
      constexpr auto kCapture = std::string_view{"OggS"};
      constexpr std::size_t kHeaderSize = 27;
      std::size_t pageOffset = 0;
      std::size_t tagsOffset = 0;
      std::size_t tagsSize = 0;
      auto packet = std::vector<std::uint8_t>{};
      std::uint8_t headerType = 0;
      std::int64_t granule = 0;
      std::uint32_t serial = 0;
      std::uint32_t sequence = 0;
      bool foundTags = false;

      while (pageOffset + kHeaderSize <= bytes.size() &&
             std::string_view{reinterpret_cast<char const*>(bytes.data() + pageOffset), kCapture.size()} == kCapture)
      {
        auto const segmentCount = static_cast<std::size_t>(bytes[pageOffset + 26]);
        REQUIRE(pageOffset + kHeaderSize + segmentCount <= bytes.size());
        std::size_t bodySize = 0;

        for (std::size_t index = 0; index < segmentCount; ++index)
        {
          bodySize += bytes[pageOffset + kHeaderSize + index];
        }

        auto const pageSize = kHeaderSize + segmentCount + bodySize;
        REQUIRE(pageOffset + pageSize <= bytes.size());
        auto const payloadOffset = pageOffset + kHeaderSize + segmentCount;

        if (bodySize >= 8 &&
            std::string_view{reinterpret_cast<char const*>(bytes.data() + payloadOffset), 8} == "OpusTags")
        {
          REQUIRE(segmentCount == 1);
          REQUIRE(bytes[pageOffset + kHeaderSize] < 255);
          foundTags = true;
          tagsOffset = pageOffset;
          tagsSize = pageSize;
          packet.assign(bytes.begin() + static_cast<std::ptrdiff_t>(payloadOffset),
                        bytes.begin() + static_cast<std::ptrdiff_t>(payloadOffset + bodySize));
          headerType = bytes[pageOffset + 5];
          granule = littleEndian64(bytes, pageOffset + 6);
          serial = littleEndian32(bytes, pageOffset + 14);
          sequence = littleEndian32(bytes, pageOffset + 18);
          break;
        }

        pageOffset += pageSize;
      }

      REQUIRE(foundTags);
      auto const vendorLength = littleEndian32(packet, 8);
      auto const countOffset = std::size_t{12} + vendorLength;
      auto const commentCount = littleEndian32(packet, countOffset);
      auto cursor = countOffset + 4;

      for (std::uint32_t index = 0; index < commentCount; ++index)
      {
        auto const length = littleEndian32(packet, cursor);
        cursor += 4 + length;
      }

      REQUIRE(cursor == packet.size());
      auto comment = "CUESHEET=" + std::string{cueValue};
      auto rebuilt =
        std::vector<std::uint8_t>{packet.begin(), packet.begin() + static_cast<std::ptrdiff_t>(countOffset)};
      auto const appendLe32 = [&rebuilt](std::uint32_t const value)
      {
        for (std::size_t index = 0; index < 4; ++index)
        {
          rebuilt.push_back(static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU));
        }
      };
      appendLe32(commentCount + 1);
      rebuilt.insert(rebuilt.end(), packet.begin() + static_cast<std::ptrdiff_t>(countOffset + 4), packet.end());
      appendLe32(static_cast<std::uint32_t>(comment.size()));
      rebuilt.insert(rebuilt.end(), comment.begin(), comment.end());
      auto const page =
        ao::test::ogg::makePage(ao::test::ogg::Page{.headerType = headerType,
                                                    .granulePosition = granule,
                                                    .serialNumber = serial,
                                                    .pageSequence = sequence,
                                                    .lacingValues = ao::test::ogg::lacingFor({rebuilt.size()}),
                                                    .payload = std::move(rebuilt)});
      auto output = std::vector<std::uint8_t>{};
      output.insert(output.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(tagsOffset));
      output.insert(output.end(), page.begin(), page.end());
      output.insert(output.end(), bytes.begin() + static_cast<std::ptrdiff_t>(tagsOffset + tagsSize), bytes.end());
      auto stream = std::ofstream{destination, std::ios::binary | std::ios::trunc};
      stream.write(reinterpret_cast<char const*>(output.data()), static_cast<std::streamsize>(output.size()));
      REQUIRE(stream.good());
    }

    void requireWholeOpusImport(library::MusicLibrary& library, std::string_view const uri)
    {
      auto planRes = LibraryScan{library}.buildPlan();
      REQUIRE(planRes);
      REQUIRE(planRes->size() == 1);
      CHECK(planRes->items().front().classification == ScanClassification::New);
      CHECK(planRes->items().front().cueDiagnostic == ScanCueDiagnostic::None);
      CHECK(planRes->items().front().errorMessage.empty());
      auto res = ScanApplyOperation{library, std::move(*planRes), {}, {}}.run();
      REQUIRE(res);
      REQUIRE(res->insertedIds.size() == 1);
      CHECK(res->failureCount == 0);
      auto transaction = library.readTransaction();
      auto optTrack = library.tracks().reader(transaction).get(res->insertedIds.front());
      REQUIRE(optTrack);
      CHECK(optTrack->metadata().title() == "Test Title");
      CHECK_FALSE(optTrack->property().segment());
      auto optManifest = library.manifest().reader(transaction).get(uri);
      REQUIRE(optManifest);
      CHECK(optManifest->segmentCount() == 0);
      CHECK(optManifest->trackId() == res->insertedIds.front());
    }
  } // namespace

  TEST_CASE("CUE scan - unchanged whole file keeps empty evidence until an external sheet appears",
            "[runtime][integration][cue-scan]")
  {
    auto fixture = CueScanFixture{};
    std::filesystem::remove(fixture.musicRoot / "image.cue");
    auto const imported = fixture.scan();
    REQUIRE(imported.insertedIds.size() == 1);

    // Matching file facts preserve the admitted whole-file result. This does not
    // count opens; the scan strace is the direct evidence that media is not reopened.
    auto unchanged = fixture.plan();
    REQUIRE(unchanged.size() == 1);
    CHECK(unchanged.items().front().classification == ScanClassification::Unchanged);
    CHECK(unchanged.items().front().cueEvidence == ScanCueEvidence{});
    CHECK(unchanged.items().front().cueLayout.empty());
    CHECK(unchanged.items().front().errorMessage.empty());
    CHECK(unchanged.items().front().cueDiagnostic == ScanCueDiagnostic::None);

    writeSheet(fixture.musicRoot / "image.cue");
    auto divided = fixture.plan();
    REQUIRE(divided.size() == 1);
    CHECK(divided.items().front().classification == ScanClassification::Unchanged);
    CHECK(divided.items().front().cueDiagnostic == ScanCueDiagnostic::WholeFileAlreadyImported);
    CHECK(divided.items().front().errorMessage.contains("whole file"));
    CHECK(divided.items().front().cueEvidence.general.size() == 1);
    CHECK(divided.items().front().cueEvidence.extensionSpecific.empty());
  }

  TEST_CASE("CUE scan - Opus embedded comments import whole and an external sheet stays unsupported",
            "[runtime][integration][cue-scan]")
  {
    auto temp = ao::test::TempDir{};
    auto const musicRoot = makeMusicRoot(temp.path());
    auto library = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");

    SECTION("A plain Opus fixture imports as one whole-file Track")
    {
      audio::test::installAudioFixture(musicRoot, "basic_metadata.opus", "song.opus");
      requireWholeOpusImport(library, "song.opus");
    }

    SECTION("A valid embedded CUESHEET does not split or reject the file")
    {
      writeOpusFixture(musicRoot / "song.opus", kSheet);
      requireWholeOpusImport(library, "song.opus");
    }

    SECTION("A malformed embedded CUESHEET is not a binding failure")
    {
      writeOpusFixture(musicRoot / "song.opus", "not a CUE sheet");
      requireWholeOpusImport(library, "song.opus");
    }

    SECTION("An external same-name sheet remains an unsupported carrier")
    {
      audio::test::installAudioFixture(musicRoot, "basic_metadata.opus", "song.opus");
      writeSheet(musicRoot / "song.cue");
      auto planRes = LibraryScan{library}.buildPlan();
      REQUIRE(planRes);
      REQUIRE(planRes->size() == 1);
      CHECK(planRes->items().front().classification == ScanClassification::Error);
      CHECK(planRes->items().front().cueDiagnostic == ScanCueDiagnostic::UnsupportedCarrier);
      CHECK(planRes->items().front().errorMessage.contains("cannot be a CUE carrier"));
      auto res = ScanApplyOperation{library, std::move(*planRes), {}, {}}.run();
      REQUIRE(res);
      CHECK(res->insertedIds.empty());
      CHECK(res->failureCount == 1);
      auto transaction = library.readTransaction();
      CHECK_FALSE(library.manifest().reader(transaction).get("song.opus"));
    }
  }

  TEST_CASE("CUE scan - both candidate precedences are evidenced and only the deciding set binds",
            "[runtime][integration][cue-scan]")
  {
    auto fixture = CueScanFixture{};
    writeSheet(fixture.musicRoot / "image.cue", "malformed lower precedence sheet");
    writeSheet(fixture.musicRoot / "image.flac.cue");

    SECTION("A single extension-specific sheet overrides an invalid general sheet")
    {
      auto plan = fixture.plan();
      REQUIRE(plan.size() == 1);
      CHECK(plan.items()[0].classification == ScanClassification::New);
      CHECK(plan.items()[0].cueEvidence.general.size() == 1);
      CHECK(plan.items()[0].cueEvidence.extensionSpecific.size() == 1);
      CHECK(plan.items()[0].cueDiagnostic == ScanCueDiagnostic::None);
      auto res = ScanApplyOperation{fixture.library, std::move(plan), {}, {}}.run();
      REQUIRE(res);
      REQUIRE(res->insertedIds.size() == 2);
      CHECK(storedTrackSpec(fixture.library, res->insertedIds[1]).title == "Third");
    }

    SECTION("Competing extension-specific spellings block import")
    {
      writeSheet(fixture.musicRoot / "image.FLAC.CUE");

      if (std::filesystem::equivalent(fixture.musicRoot / "image.flac.cue", fixture.musicRoot / "image.FLAC.CUE"))
      {
        SKIP("The filesystem cannot represent competing case spellings");
      }

      auto plan = fixture.plan();
      REQUIRE(plan.size() == 1);
      CHECK(plan.items()[0].classification == ScanClassification::Error);
      CHECK(plan.items()[0].cueDiagnostic == ScanCueDiagnostic::AmbiguousSheets);
      CHECK(plan.items()[0].cueDetail == "image.FLAC.CUE, image.flac.cue");
      CHECK(plan.items()[0].cueEvidence.extensionSpecific.size() == 2);
      CHECK(plan.items()[0].errorMessage.contains("image.flac.cue"));
      CHECK(plan.items()[0].errorMessage.contains("image.FLAC.CUE"));
      auto res = ScanApplyOperation{fixture.library, std::move(plan), {}, {}}.run();
      REQUIRE(res);
      CHECK(res->insertedIds.empty());
      auto transaction = fixture.library.readTransaction();
      CHECK_FALSE(fixture.library.manifest().reader(transaction).get("image.flac"));
    }
  }
} // namespace ao::rt::test
