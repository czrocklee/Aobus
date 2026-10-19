// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CueYamlTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/library/ScanApplyTestSupport.h"
#include <ao/Error.h>
#include <ao/FileTimestamp.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/FileManifestView.h>
#include <ao/library/FileTimeConversion.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/library/TrackWriter.h>
#include <ao/rt/library/LibraryTransfer.h>
#include <ao/utility/Hash128.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace ao::rt::test
{
  namespace
  {
    constexpr auto kOldFileFacts = std::string_view{"      file-size: 13\n      mtime: {seconds: 1, nanoseconds: 2}\n"};

    struct ManifestSnapshot final
    {
      std::uint64_t fileSize = 0;
      std::optional<FileTimestamp> optMtime{};
      std::uint64_t audioPayloadLength = 0;
      utility::Hash128 audioSignature{};
      library::FileStatus status = library::FileStatus::Available;

      bool operator==(ManifestSnapshot const&) const = default;
    };

    ManifestSnapshot manifestSnapshot(library::MusicLibrary const& library)
    {
      auto transaction = library.readTransaction();
      auto const optManifest = library.manifest().reader(transaction).get("disc.flac");
      REQUIRE(optManifest);
      return {.fileSize = optManifest->fileSize(),
              .optMtime = optManifest->mtime(),
              .audioPayloadLength = optManifest->audioPayloadLength(),
              .audioSignature = optManifest->audioSignature(),
              .status = optManifest->status()};
    }

    ManifestSnapshot filesystemBaseline(std::filesystem::path const& path)
    {
      auto snapshot = ManifestSnapshot{};

      if (std::filesystem::exists(path))
      {
        snapshot.fileSize = std::filesystem::file_size(path);
        snapshot.optMtime = ao::test::requireValue(library::lastWriteTimestamp(path));
      }

      return snapshot;
    }

    std::string baselineCarrierRecords(std::string_view facts, std::string_view title)
    {
      return cueRecord(2, 0, 44100, 176400, 44100, std::format("      title: {}\n{}", title, facts)) +
             cueRecord(9, 132300, 44100, 176400, 44100, std::format("      title: Tail\n{}", facts));
    }
  } // namespace

  TEST_CASE("CueYaml baseline - carrier restore does not inherit target identity status or omitted facts",
            "[runtime][integration][import-export][cue-yaml-baseline]")
  {
    for (bool const hasMedia : {false, true})
    {
      for (auto const facts : {std::string_view{},
                               std::string_view{"      file-size: 789\n      mtime: {seconds: 20, nanoseconds: 30}\n"},
                               std::string_view{"      file-size: 789\n      mtime: null\n"}})
      {
        CAPTURE(hasMedia, facts);
        auto const temp = ao::test::TempDir{};
        auto const root = temp.path() / "music";
        auto const mediaPath = root / "disc.flac";
        std::filesystem::create_directories(root);

        if (hasMedia)
        {
          writeScanFlacMetadataFixture(mediaPath, {"TITLE=Physical title"}, 176400);
        }

        auto oldTarget = library::test::makeTestMusicLibrary(root, temp.path() / "old-db");
        auto cleanTarget = library::test::makeTestMusicLibrary(root, temp.path() / "clean-db");
        auto const documentPath = temp.path() / "document.yaml";
        REQUIRE(importCue(oldTarget, documentPath, cueDocument(baselineCarrierRecords(kOldFileFacts, "Old"))));
        auto signature = utility::Hash128{};
        signature.bytes[0] = std::byte{0x41};

        {
          auto transaction = library::test::writeTransaction(oldTarget);
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

        auto expected = filesystemBaseline(mediaPath);

        if (!facts.empty())
        {
          expected.fileSize = 789;
          expected.optMtime = facts.contains("mtime: null") ? std::nullopt : std::optional<FileTimestamp>{{20, 30}};
        }

        auto const payload = cueDocument(baselineCarrierRecords(facts, "Portable"));
        auto const oldRes = importCue(oldTarget, documentPath, payload);
        auto const cleanRes = importCue(cleanTarget, documentPath, payload);
        REQUIRE(oldRes);
        REQUIRE(cleanRes);
        CHECK(oldRes->tracksDeleted == 2);
        CHECK(oldRes->tracksCreated == 2);
        CHECK(cleanRes->tracksCreated == 2);
        CHECK(manifestSnapshot(oldTarget) == expected);
        CHECK(manifestSnapshot(cleanTarget) == expected);
        CHECK(carrierIds(oldTarget).size() == 2);
        CHECK(carrierIds(cleanTarget).size() == 2);
        auto transaction = oldTarget.readTransaction();
        auto const optManifest = oldTarget.manifest().reader(transaction).get("disc.flac");
        REQUIRE(optManifest);
        CHECK(optManifest->totalFrames() == 176400);
        REQUIRE(optManifest->segmentTrackIds().size() == 2);
        auto const reader = oldTarget.tracks().reader(transaction);
        CHECK(reader.entryCount() == 2);
        auto const optFirst = reader.get(optManifest->segmentTrackIds().front());
        REQUIRE(optFirst);
        CHECK(optFirst->metadata().title() == "Portable");
        REQUIRE(optFirst->property().segment());
        CHECK(optFirst->property().segment()->ordinal == 2);
      }
    }
  }

  TEST_CASE("CueYaml baseline - whole-file restore seeds omitted facts independently of the target row",
            "[runtime][integration][import-export][cue-yaml-baseline]")
  {
    for (bool const hasMedia : {false, true})
    {
      for (auto const mode : {ExportMode::Full, ExportMode::Delta, ExportMode::Metadata})
      {
        CAPTURE(hasMedia, mode);
        auto const temp = ao::test::TempDir{};
        auto const root = temp.path() / "music";
        auto const mediaPath = root / "disc.flac";
        std::filesystem::create_directories(root);

        if (hasMedia)
        {
          writeScanFlacMetadataFixture(mediaPath, {"TITLE=Physical title"});
        }

        auto oldTarget = library::test::makeTestMusicLibrary(root, temp.path() / "old-db");
        auto cleanTarget = library::test::makeTestMusicLibrary(root, temp.path() / "clean-db");
        auto const documentPath = temp.path() / "document.yaml";
        REQUIRE(importCue(oldTarget,
                          documentPath,
                          cueDocument(std::format("    - uri: disc.flac\n      title: Old\n{}", kOldFileFacts))));
        auto const expected = filesystemBaseline(mediaPath);
        auto const payload = cueDocument("    - uri: disc.flac\n      title: Restored\n", mode);
        REQUIRE(importCue(oldTarget, documentPath, payload));
        REQUIRE(importCue(cleanTarget, documentPath, payload));
        CHECK(manifestSnapshot(oldTarget) == expected);
        CHECK(manifestSnapshot(cleanTarget) == expected);
      }
    }
  }

  TEST_CASE("CueYaml baseline - whole-file merge preserves omitted target manifest facts",
            "[runtime][integration][import-export][cue-yaml-baseline]")
  {
    for (auto const mode : {ExportMode::Full, ExportMode::Delta, ExportMode::Metadata})
    {
      CAPTURE(mode);
      auto const temp = ao::test::TempDir{};
      writeScanFlacMetadataFixture(temp.path() / "disc.flac", {"TITLE=Physical title"});
      auto library = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
      auto const path = temp.path() / "document.yaml";
      REQUIRE(importCue(
        library, path, cueDocument(std::format("    - uri: disc.flac\n      title: Old\n{}", kOldFileFacts))));
      auto const before = manifestSnapshot(library);
      auto const res =
        importCue(library, path, cueDocument("    - uri: disc.flac\n      title: Merged\n", mode), ImportMode::Merge);
      REQUIRE(res);
      CHECK(res->tracksUpdated == 1);
      CHECK(res->tracksCreated == 0);
      CHECK(manifestSnapshot(library) == before);
      CHECK(before.fileSize == 13);
      CHECK(before.optMtime == FileTimestamp{1, 2});
      auto transaction = library.readTransaction();
      auto const optManifest = library.manifest().reader(transaction).get("disc.flac");
      REQUIRE(optManifest);
      auto const optView = library.tracks().reader(transaction).get(optManifest->trackId());
      REQUIRE(optView);
      CHECK(optView->metadata().title() == "Merged");
    }
  }
} // namespace ao::rt::test
