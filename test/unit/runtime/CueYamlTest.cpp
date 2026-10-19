// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CueYamlTestSupport.h"
#include "runtime/library/LibraryYamlExporter.h"
#include "runtime/library/LibraryYamlImporter.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/library/ScanApplyTestSupport.h"
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/library/TrackWriter.h>
#include <ao/rt/library/LibraryTransfer.h>
#include <ao/utility/Hash128.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    void checkSparseCarrier(library::MusicLibrary const& library, std::vector<TrackId> const& ids)
    {
      REQUIRE(ids.size() == 2);
      auto transaction = library.readTransaction();
      auto reader = library.tracks().reader(transaction);
      auto const optFirst = reader.get(ids[0]);
      auto const optSecond = reader.get(ids[1]);
      REQUIRE(optFirst);
      REQUIRE(optSecond);
      REQUIRE(optFirst->property().segment());
      REQUIRE(optSecond->property().segment());
      CHECK(*optFirst->property().segment() == library::TrackSegment{.startFrame = 0, .frameCount = 588, .ordinal = 2});
      CHECK(*optSecond->property().segment() ==
            library::TrackSegment{.startFrame = 588, .frameCount = 1, .ordinal = 7});
      CHECK(optFirst->property().sampleRate() == SampleRate{44100});
      CHECK(optSecond->property().sampleRate() == SampleRate{44100});
      CHECK(optFirst->property().duration() == std::chrono::milliseconds{13});
      CHECK(optSecond->property().duration() == std::chrono::milliseconds{0});
      auto const optManifest = library.manifest().reader(transaction).get("disc.flac");
      REQUIRE(optManifest);
      CHECK(optManifest->totalFrames() == 589);
    }
  } // namespace

  TEST_CASE("CueYaml - every export mode preserves sparse segment identities and list order offline",
            "[runtime][integration][import-export][cue]")
  {
    auto const temp = ao::test::TempDir{};
    auto source = library::test::makeTestMusicLibrary(temp.path() / "absent-source", temp.path() / "source-db");
    auto const path = temp.path() / "document.yaml";
    auto const records = cueRecord(7, 588, 1, 589, 44100, "      id: 70\n      title: Tail\n") +
                         cueRecord(2, 0, 588, 589, 44100, "      id: 20\n      title: Opening\n", "./disc.flac");
    auto const lists = std::string_view{"\n    - id: 1\n      name: Disc\n      order: [70, 20]\n"};
    REQUIRE(importCue(source, path, cueDocument(records, ExportMode::Full, lists)));
    checkSparseCarrier(source, carrierIds(source));

    for (auto const mode : {ExportMode::Full, ExportMode::Delta, ExportMode::Metadata, ExportMode::ListOnly})
    {
      CAPTURE(mode);
      auto const exportPath = temp.path() / "export.yaml";
      REQUIRE(LibraryYamlExporter{source}.exportToYaml(exportPath, mode));
      auto const exported = ao::test::readFile(exportPath);
      CHECK(exported.contains("segment:") == (mode != ExportMode::ListOnly));
      CHECK(exported.contains("ordinal:"));
      auto const targetDirectory = temp.path() / std::string{exportModeName(mode)};
      auto target = library::test::makeTestMusicLibrary(temp.path() / "absent-target", targetDirectory);

      if (mode == ExportMode::ListOnly)
      {
        // Occupy a target ID before creating the carrier, so URI/ordinal ranks
        // cannot accidentally succeed by using the producer's IDs.
        library::test::addTrack(target, library::test::makeEmptyTrackSpec("other.flac"));
        REQUIRE(importCue(target, path, cueDocument(records), ImportMode::Merge));
      }

      auto const reportRes = LibraryYamlImporter{target}.importFromYamlOffline(exportPath, ImportMode::Restore);
      REQUIRE(reportRes);
      CHECK(reportRes->tracksCreated == (mode == ExportMode::ListOnly ? 0U : 2U));
      CHECK(reportRes->listsCreated == 1);
      CHECK(reportRes->danglingReferencesIgnored == 0);
      auto const ids = carrierIds(target);
      checkSparseCarrier(target, ids);
      auto transaction = target.readTransaction();
      std::size_t listCount = 0;

      for (auto const& [id, view] : target.lists().reader(transaction))
      {
        CHECK(id != kInvalidListId);
        CHECK(view.name() == "Disc");
        auto const order = view.orderTrackIds();
        REQUIRE(order.size() == 2);
        CHECK(order[0] == ids[1]);
        CHECK(order[1] == ids[0]);
        ++listCount;
      }

      CHECK(listCount == 1);
    }
  }

  TEST_CASE("CueYaml - malformed structural facts reject the complete restore before writes",
            "[runtime][unit][import-export][cue]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const keptId = library::test::addTrack(library, library::test::TrackSpec{.title = "Kept", .uri = "kept.flac"});
    auto const revision = library.libraryRevision(library.readTransaction());
    auto const dictionarySize = library.dictionary().size();
    auto const path = temp.path() / "document.yaml";
    auto const valid = cueRecord(2, 0, 44100);
    auto const invalidRecords = std::vector<std::string>{
      "    - uri: disc.flac\n      segment: {}\n",
      "    - uri: disc.flac\n      segment: []\n",
      "    - uri: disc.flac\n      segment: {ordinal: 2, start-frame: 0, frame-count: 1, total-frames: 1}\n",
      std::string{"    - uri: disc.flac\n      segment: {ordinal: 2, start-frame: -1, frame-count: 1, total-frames: 1, "
                  "sample-rate: 44100}\n"},
      std::string{"    - uri: disc.flac\n      segment: {ordinal: 2, start-frame: 0, frame-count: 1, total-frames: 1, "
                  "sample-rate: "
                  "44100, extra: 1}\n"},
      std::string{"    - uri: disc.flac\n      segment: {ordinal: 2, ordinal: 3, start-frame: 0, frame-count: 1, "
                  "total-frames: 1, "
                  "sample-rate: 44100}\n"},
      cueRecord(0, 0, 1),
      cueRecord(100, 0, 1),
      cueRecord(2, 0, 0),
      cueRecord(2, 0, 1, 0),
      cueRecord(2, 0, 1, 132300, 0),
      cueRecord(2, 132300, 1),
      cueRecord(2, 18446744073709551615ULL, 1, 18446744073709551615ULL),
      cueRecord(2, 0, 18446744073709551615ULL, 18446744073709551615ULL, 1),
      cueRecord(2, 0, 44100, 132300, 44100, "      duration: 999\n"),
      cueRecord(2, 0, 44100, 132300, 44100, "      duration: nope\n"),
      cueRecord(2, 0, 44100, 132300, 44100, "      sample-rate: 48000\n"),
      cueRecord(2, 0, 44100, 132300, 44100, "      sample-rate: nope\n"),
      valid + "    - uri: ./disc.flac\n",
      valid + cueRecord(2, 44100, 44100),
      valid + cueRecord(7, 22050, 44100),
      valid + cueRecord(7, 44100, 44100, 176400),
      valid + cueRecord(7, 44100, 44100, 132300, 48000),
      cueRecord(2, 0, 44100, 132300, 44100, "      file-size: 1\n") +
        cueRecord(7, 44100, 44100, 132300, 44100, "      file-size: 2\n"),
      cueRecord(2, 0, 44100, 132300, 44100, "      mtime: null\n") +
        cueRecord(7, 44100, 44100, 132300, 44100, "      mtime: {seconds: 0, nanoseconds: 0}\n")};

    for (auto const& records : invalidRecords)
    {
      CAPTURE(records);
      // A valid earlier record must not survive the later malformed carrier.
      auto const res =
        importCue(library, path, cueDocument("    - uri: new.flac\n      title: Never written\n" + records));
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
      auto transaction = library.readTransaction();
      CHECK(library.libraryRevision(transaction) == revision);
      CHECK(library.dictionary().size() == dictionarySize);
      auto const reader = library.tracks().reader(transaction);
      CHECK(reader.entryCount() == 1);
      auto const optKept = reader.get(keptId);
      REQUIRE(optKept);
      CHECK(optKept->metadata().title() == "Kept");
    }
  }

  TEST_CASE("CueYaml - segments name FLAC or WAV carriers and duplicate URIs report their form",
            "[runtime][unit][import-export][cue]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const path = temp.path() / "document.yaml";
    auto const rejected = [&](std::string const& tracks)
    {
      auto const res = importCue(library, path, cueDocument(tracks));
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
      CHECK(library.tracks().reader(library.readTransaction()).entryCount() == 0);
      return res.error().message;
    };

    // CUE binding divides only FLAC and WAV, so a restore cannot create another carrier.
    for (auto const* const uri : {"disc.mp3", "disc.m4a", "disc.opus"})
    {
      CAPTURE(uri);
      CHECK(rejected(cueRecord(1, 0, 588, 1176, 44100, "", uri)).contains("not a FLAC or WAV carrier"));
    }

    CHECK(rejected("    - uri: song.flac\n    - uri: song.flac\n")
            .contains("Duplicate canonical track URI 'song.flac' in payload"));
    CHECK(rejected("    - uri: disc.flac\n" + cueRecord(1, 0, 588)).contains("mixes whole-file and carrier records"));
    CHECK(rejected(cueRecord(1, 0, 588) + "    - uri: disc.flac\n").contains("mixes whole-file and carrier records"));

    // The first invalid group in record order is the one reported, whatever the hash order.
    auto duplicates = std::string{};

    for (auto const* const name : {"z", "m", "a", "q", "c", "x", "b", "k"})
    {
      duplicates += std::format("    - uri: {0}.flac\n    - uri: {0}.flac\n", name);
    }

    CHECK(rejected(duplicates).contains("Duplicate canonical track URI 'z.flac' in payload"));

    // The extension check ignores ASCII case, like audio-file recognition.
    REQUIRE(importCue(library, path, cueDocument(cueRecord(1, 0, 588, 1176, 44100, "", "DISC.WAV"))));
  }

  TEST_CASE("CueYaml - merge retains matched IDs and validates additions against retained members",
            "[runtime][integration][import-export][cue]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = library::test::makeTestMusicLibrary(temp.path() / "missing", temp.path() / "db");
    auto const path = temp.path() / "document.yaml";
    REQUIRE(
      importCue(library, path, cueDocument(cueRecord(2, 0, 44100, 176400) + cueRecord(9, 132300, 44100, 176400))));
    auto const oldIds = carrierIds(library);
    REQUIRE(oldIds.size() == 2);

    auto signature = utility::Hash128{};
    signature.bytes[0] = std::byte{0x41};

    {
      auto transaction = library::test::writeTransaction(library);
      REQUIRE(transaction.apply(
        [&](library::LibraryWrite& write) -> Result<>
        {
          auto const optManifest = write.tracks().manifest("disc.flac");
          REQUIRE(optManifest);
          auto manifest = library::FileManifestBuilder::fromView(*optManifest);
          manifest.audioPayloadLength(12345).audioSignature(signature).status(library::FileStatus::Missing);
          return write.tracks().updateManifest("disc.flac", manifest);
        }));
      REQUIRE(transaction.commit());
    }

    auto const records = cueRecord(7, 88200, 44100, 176400, 44100, "      id: 70\n      title: Added\n") +
                         cueRecord(4, 44100, 44100, 176400, 44100, "      id: 40\n      title: Middle\n") +
                         cueRecord(2, 0, 44100, 176400, 44100, "      id: 20\n      title: Updated\n");
    auto const lists =
      std::string_view{"\n    - id: 1\n      name: Partial\n      order: [70, 20, {uri: disc.flac, ordinal: 9}, 40]\n"};
    auto const res = importCue(library, path, cueDocument(records, ExportMode::Full, lists), ImportMode::Merge);
    REQUIRE(res);
    CHECK(res->tracksCreated == 2);
    CHECK(res->tracksUpdated == 1);
    auto const ids = carrierIds(library);
    REQUIRE(ids.size() == 4);
    CHECK(ids[0] == oldIds[0]);
    CHECK(ids[3] == oldIds[1]);
    auto transaction = library.readTransaction();
    auto const reader = library.tracks().reader(transaction);
    CHECK(reader.get(ids[0])->metadata().title() == "Updated");
    CHECK(reader.get(ids[1])->metadata().title() == "Middle");
    CHECK(reader.get(ids[2])->metadata().title() == "Added");
    CHECK(reader.get(ids[3])->property().segment()->ordinal == 9);
    auto const optManifest = library.manifest().reader(transaction).get("disc.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->audioPayloadLength() == 12345);
    CHECK(optManifest->audioSignature() == signature);
    CHECK(optManifest->status() == library::FileStatus::Missing);

    for (auto const& [id, view] : library.lists().reader(transaction))
    {
      CHECK(id != kInvalidListId);
      auto const order = view.orderTrackIds();
      REQUIRE(order.size() == 4);
      CHECK(order[0] == ids[2]);
      CHECK(order[1] == ids[0]);
      CHECK(order[2] == ids[3]);
      CHECK(order[3] == ids[1]);
    }
  }

  TEST_CASE("CueYaml - conflicting merge forms extents rates and ranges roll back atomically",
            "[runtime][integration][import-export][cue]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = library::test::makeTestMusicLibrary(temp.path() / "missing", temp.path() / "db");
    auto const path = temp.path() / "document.yaml";
    REQUIRE(
      importCue(library, path, cueDocument(cueRecord(2, 0, 44100) + "    - uri: kept.flac\n      title: Kept\n")));
    auto const ids = carrierIds(library);
    auto const revision = library.libraryRevision(library.readTransaction());
    auto const invalid =
      std::vector<std::string>{cueRecord(7, 44100, 44100, 176400), // No ordinal matches; extent still must agree.
                               cueRecord(7, 44100, 44100, 132300, 48000),
                               cueRecord(7, 22050, 44100), // Payload alone is valid but overlaps a retained member.
                               cueRecord(2, 1, 44100),
                               "    - uri: disc.flac\n      title: Cannot become whole\n",
                               cueRecord(7, 44100, 44100, 132300, 44100, "", "kept.flac")};

    for (auto const& records : invalid)
    {
      CAPTURE(records);
      auto const* earlierRecord = records.contains("uri: kept.flac")
                                    ? "    - uri: new.flac\n      title: Never committed\n"
                                    : "    - uri: kept.flac\n      title: Never committed\n";
      auto const res = importCue(library, path, cueDocument(earlierRecord + records), ImportMode::Merge);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
      CHECK(carrierIds(library) == ids);
      auto transaction = library.readTransaction();
      CHECK(library.libraryRevision(transaction) == revision);
      auto const reader = library.tracks().reader(transaction);
      CHECK(reader.entryCount() == 2);
      auto const optManifest = library.manifest().reader(transaction).get("kept.flac");
      REQUIRE(optManifest);
      CHECK(reader.get(optManifest->trackId())->metadata().title() == "Kept");
    }
  }

  TEST_CASE("CueYaml - URI ranks without ordinal resolve only whole files",
            "[runtime][integration][import-export][cue]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const path = temp.path() / "document.yaml";
    REQUIRE(importCue(library, path, cueDocument(cueRecord(7, 44100, 44100) + "    - uri: whole.flac\n")));
    auto const ids = carrierIds(library);
    auto const document = std::format("version: {}\nexport-mode: list-only\nlibrary:\n  lists:\n"
                                      "    - id: 1\n      name: References\n      order:\n"
                                      "        - uri: ./disc.flac\n          ordinal: 7\n"
                                      "        - uri: disc.flac\n"
                                      "        - uri: disc.flac\n          ordinal: 2\n"
                                      "        - uri: whole.flac\n          ordinal: 7\n"
                                      "        - uri: whole.flac\n"
                                      "        - uri: disc.flac\n          ordinal: 7\n",
                                      kYamlFormatVersion);
    auto const res = importCue(library, path, document);
    REQUIRE(res);
    CHECK(res->danglingReferencesIgnored == 3);
    CHECK(res->tracksDeleted == 0);
    auto transaction = library.readTransaction();
    auto const wholeId = library.manifest().reader(transaction).get("whole.flac")->trackId();

    for (auto const& [id, view] : library.lists().reader(transaction))
    {
      CHECK(id != kInvalidListId);
      auto const order = view.orderTrackIds();
      REQUIRE(order.size() == 2);
      CHECK(order[0] == ids[0]);
      CHECK(order[1] == wholeId);
    }
  }

  TEST_CASE("CueYaml - full restore trusts structural facts without opening readable audio",
            "[runtime][integration][import-export][cue]")
  {
    auto const temp = ao::test::TempDir{};
    writeScanFlacMetadataFixture(temp.path() / "disc.flac", {"TITLE=Do not read"}, 88200);
    auto library = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const res =
      importCue(library,
                temp.path() / "full.yaml",
                cueDocument(cueRecord(2, 0, 588, 589, 48000, "      duration: 12\n      sample-rate: 48000\n") +
                            cueRecord(7, 588, 1, 589, 48000)));
    REQUIRE(res);
    CHECK(res->tracksCreated == 2);
    auto const ids = carrierIds(library);
    REQUIRE(ids.size() == 2);
    auto transaction = library.readTransaction();
    auto const reader = library.tracks().reader(transaction);
    auto const optView = reader.get(ids[0]);
    REQUIRE(optView);
    CHECK(optView->metadata().title().empty());
    CHECK(optView->property().sampleRate() == SampleRate{48000});
    CHECK(optView->property().duration() == std::chrono::milliseconds{12});
    CHECK(library.manifest().reader(transaction).get("disc.flac")->totalFrames() == 589);
  }

  TEST_CASE("CueYaml - malformed ordinal rank references reject instead of falling back to whole files",
            "[runtime][unit][import-export][cue]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const keptId = library::test::addTrack(library, library::test::makeEmptyTrackSpec("kept.flac"));
    auto const path = temp.path() / "document.yaml";

    for (auto const* reference : {"{id: 1, ordinal: 2}",
                                  "{ordinal: 2}",
                                  "{uri: disc.flac, ordinal: 0}",
                                  "{uri: disc.flac, ordinal: 100}",
                                  "{uri: disc.flac, ordinal: nope}",
                                  "{uri: disc.flac, ordinal: [2]}",
                                  "{uri: disc.flac, ordinal: 2, ordinal: 7}"})
    {
      CAPTURE(reference);
      auto const document = std::format("version: {}\nexport-mode: list-only\nlibrary:\n  lists:\n"
                                        "    - id: 1\n      name: Invalid\n      order: [{}]\n",
                                        kYamlFormatVersion,
                                        reference);
      auto const res = importCue(library, path, document);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
      auto transaction = library.readTransaction();
      auto const optView = library.tracks().reader(transaction).get(keptId);
      REQUIRE(optView);
      CHECK(optView->property().uri() == "kept.flac");
      CHECK(library.lists().reader(transaction).begin() == library.lists().reader(transaction).end());
    }
  }

  TEST_CASE("CueYaml - delta and metadata enforce readable exact carrier facts without changing segment duration",
            "[runtime][integration][import-export][cue]")
  {
    auto const temp = ao::test::TempDir{};
    auto const path = temp.path() / "document.yaml";

    for (auto const mode : {ExportMode::Delta, ExportMode::Metadata})
    {
      CAPTURE(mode);
      auto const directory = temp.path() / std::string{exportModeName(mode)};
      std::filesystem::create_directories(directory);
      writeScanFlacMetadataFixture(directory / "disc.flac", {"TITLE=Carrier title"}, 44100);
      auto library = library::test::makeTestMusicLibrary(directory, directory / "db");
      auto const validRecords = cueRecord(2, 0, 588, 44100, 44100, "      duration: 13\n      title: Curated\n") +
                                cueRecord(7, 588, 43512, 44100);
      auto const validRes = importCue(library, path, cueDocument(validRecords, mode));
      REQUIRE(validRes);
      auto const ids = carrierIds(library);
      REQUIRE(ids.size() == 2);

      {
        auto transaction = library.readTransaction();
        auto const optView = library.tracks().reader(transaction).get(ids[0]);
        REQUIRE(optView);
        CHECK(optView->property().duration() == std::chrono::milliseconds{13});
        CHECK(optView->property().sampleRate() == SampleRate{44100});
      }

      auto const mergeRes = importCue(library, path, cueDocument(cueRecord(2, 0, 588, 44100), mode), ImportMode::Merge);
      REQUIRE(mergeRes);
      CHECK(mergeRes->tracksUpdated == 1);
      CHECK(mergeRes->tracksCreated == 0);
      CHECK(carrierIds(library) == ids);

      {
        auto transaction = library.readTransaction();
        auto const optView = library.tracks().reader(transaction).get(ids[0]);
        REQUIRE(optView);
        CHECK(optView->metadata().title() == "Curated");
        CHECK(optView->property().duration() == std::chrono::milliseconds{13});
        CHECK(optView->property().sampleRate() == SampleRate{44100});
      }

      auto const revision = library.libraryRevision(library.readTransaction());

      for (auto const strategy : {ImportMode::Restore, ImportMode::Merge})
      {
        for (auto const& records : {cueRecord(9, 0, 588, 88200), cueRecord(9, 0, 588, 44100, 48000)})
        {
          auto const res = importCue(library, path, cueDocument(records, mode), strategy);
          REQUIRE_FALSE(res);
          CHECK(res.error().code == Error::Code::FormatRejected);
          CHECK(carrierIds(library) == ids);
          CHECK(library.libraryRevision(library.readTransaction()) == revision);
        }
      }

      // A physical change is rejected against the same stored structure too.
      writeScanFlacMetadataFixture(directory / "disc.flac", {}, 88200);
      auto const changedRes = importCue(library, path, cueDocument(validRecords, mode), ImportMode::Merge);
      REQUIRE_FALSE(changedRes);
      CHECK(changedRes.error().code == Error::Code::FormatRejected);
      CHECK(carrierIds(library) == ids);
    }
  }
} // namespace ao::rt::test
