// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CueYamlTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include "test/library/WritableLibraryTestSupport.h"
#include "test/runtime/library/ScanApplyTestSupport.h"
#include <ao/Error.h>
#include <ao/FileTimestamp.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/FileManifestView.h>
#include <ao/library/FileTimeConversion.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/library/TrackWriter.h>
#include <ao/rt/library/LibraryTransfer.h>
#include <ao/utility/Hash128.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
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
    constexpr std::uint64_t kKnownPayloadLength = 12345;
    constexpr auto kKnownSignature = utility::Hash128{.bytes = {std::byte{0x41}}};

    void setManifestEvidence(library::MusicLibrary& library,
                             std::uint64_t payloadLength,
                             utility::Hash128 signature,
                             library::FileStatus status = library::FileStatus::Missing)
    {
      auto transaction = library::test::writeTransaction(library);
      REQUIRE(transaction.apply(
        [&](library::LibraryWrite& write) -> Result<>
        {
          auto const optManifest = write.tracks().manifest("disc.flac");
          REQUIRE(optManifest);
          auto manifest = library::FileManifestBuilder::fromView(*optManifest);
          manifest.audioPayloadLength(payloadLength).audioSignature(signature).status(status);
          return write.tracks().updateManifest("disc.flac", manifest);
        }));
      REQUIRE(transaction.commit());
    }

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

  TEST_CASE("CueYaml baseline - carrier restore does not inherit target identity status or omitted facts")
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
        setManifestEvidence(oldTarget, kKnownPayloadLength, kKnownSignature);

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

  TEST_CASE("CueYaml baseline - whole-file restore seeds omitted facts independently of the target row")
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
        setManifestEvidence(oldTarget, kKnownPayloadLength, kKnownSignature);
        auto const expected = filesystemBaseline(mediaPath);
        auto const payload = cueDocument("    - uri: disc.flac\n      title: Restored\n", mode);
        REQUIRE(importCue(oldTarget, documentPath, payload));
        REQUIRE(importCue(cleanTarget, documentPath, payload));
        CHECK(manifestSnapshot(oldTarget) == expected);
        CHECK(manifestSnapshot(cleanTarget) == expected);
      }
    }
  }

  TEST_CASE("CueYaml baseline - whole-file merge preserves omitted target manifest facts")
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
      setManifestEvidence(library, kKnownPayloadLength, kKnownSignature);
      auto const res =
        importCue(library, path, cueDocument("    - uri: disc.flac\n      title: Merged\n", mode), ImportMode::Merge);
      REQUIRE(res);
      CHECK(res->tracksUpdated == 1);
      CHECK(res->tracksCreated == 0);
      auto const after = manifestSnapshot(library);
      CHECK(after.fileSize == 13);
      CHECK(after.optMtime == FileTimestamp{1, 2});
      CHECK(after.audioPayloadLength == kKnownPayloadLength);
      CHECK(after.audioSignature == kKnownSignature);
      CHECK(after.status == library::FileStatus::Missing);
      auto transaction = library.readTransaction();
      auto const optManifest = library.manifest().reader(transaction).get("disc.flac");
      REQUIRE(optManifest);
      auto const optView = library.tracks().reader(transaction).get(optManifest->trackId());
      REQUIRE(optView);
      CHECK(optView->metadata().title() == "Merged");
    }
  }

  TEST_CASE("CueYaml baseline - whole-file merge clears the fingerprint only when supplied facts differ")
  {
    struct MergeCase final
    {
      std::string_view facts;
      std::uint64_t expectedSize = 13;
      std::optional<FileTimestamp> optExpectedMtime = FileTimestamp{1, 2};
      bool preservesIdentity = false;
      std::string_view baseline = kOldFileFacts;
      library::FileStatus status = library::FileStatus::Missing;
    };

    constexpr auto kNullFileFacts = std::string_view{"      file-size: 13\n      mtime: null\n"};
    constexpr auto kEpochFileFacts =
      std::string_view{"      file-size: 13\n      mtime: {seconds: 0, nanoseconds: 0}\n"};
    constexpr auto kCases = std::to_array<MergeCase>({
      {.facts = kOldFileFacts, .preservesIdentity = true, .status = library::FileStatus::Available},
      {.facts = "      file-size: 14\n", .expectedSize = 14},
      {.facts = "      mtime: {seconds: 1, nanoseconds: 3}\n", .optExpectedMtime = FileTimestamp{1, 3}},
      {.facts = "      mtime: null\n", .optExpectedMtime = std::nullopt, .baseline = kEpochFileFacts},
      {.facts = "      mtime: {seconds: 0, nanoseconds: 0}\n",
       .optExpectedMtime = FileTimestamp{},
       .baseline = kNullFileFacts},
      {.facts = "      mtime: null\n",
       .optExpectedMtime = std::nullopt,
       .preservesIdentity = true,
       .baseline = kNullFileFacts},
    });

    for (auto const& testCase : kCases)
    {
      CAPTURE(testCase.baseline, testCase.facts);
      auto const temp = ao::test::TempDir{};
      auto library = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
      auto const path = temp.path() / "document.yaml";
      auto const baseline = std::format("    - uri: disc.flac\n      title: Old\n{}", testCase.baseline);
      REQUIRE(importCue(library, path, cueDocument(baseline)));
      setManifestEvidence(library, kKnownPayloadLength, kKnownSignature, testCase.status);

      auto const records = std::format("    - uri: disc.flac\n      title: Merged\n{}", testCase.facts);
      auto const res = importCue(library, path, cueDocument(records), ImportMode::Merge);
      REQUIRE(res);
      CHECK(res->tracksUpdated == 1);
      CHECK(res->tracksCreated == 0);
      auto const after = manifestSnapshot(library);
      CHECK(after.fileSize == testCase.expectedSize);
      CHECK(after.optMtime == testCase.optExpectedMtime);
      CHECK(after.status == testCase.status);
      CHECK(after.audioPayloadLength == (testCase.preservesIdentity ? kKnownPayloadLength : 0));
      CHECK(after.audioSignature == (testCase.preservesIdentity ? kKnownSignature : utility::Hash128{}));
    }
  }

  TEST_CASE("CueYaml baseline - equal merge facts leave a readable file's fingerprint pending")
  {
    auto const temp = ao::test::TempDir{};
    auto const mediaPath = temp.path() / "disc.flac";
    writeScanFlacMetadataFixture(mediaPath, {"TITLE=Physical title"});
    auto const expected = filesystemBaseline(mediaPath);
    auto library = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const path = temp.path() / "document.yaml";
    REQUIRE(importCue(library, path, cueDocument("    - uri: disc.flac\n      title: Old\n")));
    REQUIRE(manifestSnapshot(library) == expected);

    auto const records = std::format("    - uri: disc.flac\n      file-size: {}\n", expected.fileSize);
    auto const res = importCue(library, path, cueDocument(records), ImportMode::Merge);
    REQUIRE(res);
    CHECK(res->tracksUpdated == 1);
    CHECK(manifestSnapshot(library) == expected);
  }

  TEST_CASE("CueYaml baseline - appending to a carrier keeps its extent and overlays the shared file facts")
  {
    auto const temp = ao::test::TempDir{};
    auto library = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const path = temp.path() / "document.yaml";
    REQUIRE(importCue(library, path, cueDocument(baselineCarrierRecords(kOldFileFacts, "Old"))));
    auto const oldIds = carrierIds(library);
    REQUIRE(oldIds.size() == 2);

    auto records = std::string{};
    std::uint64_t updatedCount = 0;
    auto expected = ManifestSnapshot{.fileSize = 13, .optMtime = FileTimestamp{1, 2}};

    SECTION("append only with equal facts keeps the missing carrier's fingerprint")
    {
      records = cueRecord(4, 44100, 44100, 176400, 44100, std::format("      title: Added\n{}", kOldFileFacts));
      expected.audioPayloadLength = kKnownPayloadLength;
      expected.audioSignature = kKnownSignature;
      expected.status = library::FileStatus::Missing;
    }

    SECTION("updating and appending with a different size clears the fingerprint but keeps error status")
    {
      records = cueRecord(4, 44100, 44100, 176400, 44100, "      title: Added\n      file-size: 14\n") +
                cueRecord(2, 0, 44100, 176400, 44100, "      title: Merged\n      file-size: 14\n");
      updatedCount = 1;
      expected.fileSize = 14;
      expected.status = library::FileStatus::Error;
    }

    setManifestEvidence(library, kKnownPayloadLength, kKnownSignature, expected.status);
    auto const res = importCue(library, path, cueDocument(records), ImportMode::Merge);
    REQUIRE(res);
    CHECK(res->tracksCreated == 1);
    CHECK(res->tracksUpdated == updatedCount);
    CHECK(manifestSnapshot(library) == expected);

    auto const ids = carrierIds(library);
    REQUIRE(ids.size() == 3);
    CHECK(ids.front() == oldIds.front());
    CHECK(ids.back() == oldIds.back());
    auto transaction = library.readTransaction();
    auto const optManifest = library.manifest().reader(transaction).get("disc.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->totalFrames() == 176400);
    auto const optTail = library.tracks().reader(transaction).get(oldIds.back());
    REQUIRE(optTail);
    CHECK(optTail->metadata().title() == "Tail");
    CHECK(optTail->property().segment() ==
          library::TrackSegment{.startFrame = 132300, .frameCount = 44100, .ordinal = 9});
  }
} // namespace ao::rt::test
