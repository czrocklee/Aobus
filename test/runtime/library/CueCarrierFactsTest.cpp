// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/ScanApplyOperation.h"
#include "test/TestFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include "test/runtime/CueYamlTestSupport.h"
#include "test/runtime/library/ScanApplyTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/ScanPlan.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
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
    constexpr auto kSheet = "FILE ignored.wav WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"
                            "TRACK 03 AUDIO\nINDEX 01 00:00:01\n";

    void writeFactsSheet(std::filesystem::path const& path)
    {
      auto output = std::ofstream{path, std::ios::binary};
      output << kSheet;
      REQUIRE(output.good());
    }

    struct CodecHints final
    {
      std::string_view first{};
      std::string_view second{};
      AudioCodec firstCodec = AudioCodec::Unknown;
      AudioCodec secondCodec = AudioCodec::Unknown;
    };
  } // namespace

  TEST_CASE("CUE carrier facts - physical media determines capability independently of member codec hints",
            "[runtime][integration][cue-carrier-facts]")
  {
    auto const temp = ao::test::TempDir{};
    auto const root = temp.path() / "music";
    std::filesystem::create_directories(root);
    writeScanFlacMetadataFixture(root / "disc.flac", {"TITLE=Physical carrier"}, 88200);
    writeFactsSheet(root / "disc.cue");
    auto library = library::test::makeTestMusicLibrary(root, temp.path() / "db");
    auto const variants = std::array{
      CodecHints{.first = "      codec: FLAC\n",
                 .second = "      codec: FLAC\n",
                 .firstCodec = AudioCodec::Flac,
                 .secondCodec = AudioCodec::Flac},
      CodecHints{.first = "      codec: FLAC\n",
                 .second = "      codec: OPUS\n",
                 .firstCodec = AudioCodec::Flac,
                 .secondCodec = AudioCodec::Opus},
      CodecHints{.first = "      codec: OPUS\n",
                 .second = "      codec: FLAC\n",
                 .firstCodec = AudioCodec::Opus,
                 .secondCodec = AudioCodec::Flac},
      CodecHints{.first = "      codec: OPUS\n",
                 .second = "      codec: OPUS\n",
                 .firstCodec = AudioCodec::Opus,
                 .secondCodec = AudioCodec::Opus},
      CodecHints{.first = "      codec: UNKNOWN\n", .second = "      codec: UNKNOWN\n"},
      CodecHints{},
    };

    for (auto const& hints : variants)
    {
      CAPTURE(hints.first, hints.second);
      auto const document = cueDocument(cueRecord(1, 0, 588, 88200, 44100, hints.first) +
                                        cueRecord(3, 588, 87612, 88200, 44100, hints.second));
      auto const restoredRes = importCue(library, temp.path() / "restore.yaml", document);
      REQUIRE(restoredRes);
      REQUIRE(restoredRes->tracksCreated == 2);
      auto const ids = carrierIds(library);
      REQUIRE(ids.size() == 2);
      auto const revision = library.libraryRevision(library.readTransaction());
      auto planRes = LibraryScan{library}.buildPlan();
      REQUIRE(planRes);
      REQUIRE(planRes->size() == 1);
      auto const& item = planRes->items().front();
      CHECK(item.classification == ScanClassification::Unchanged);
      CHECK(item.cueDiagnostic == ScanCueDiagnostic::None);
      CHECK(item.errorMessage.empty());
      CHECK(item.hasExactMediaFacts);
      CHECK(item.totalFrames == 88200);
      CHECK(item.sampleRate == 44100);
      CHECK(item.cueLayout ==
            std::vector<library::TrackSegment>{{.startFrame = 0, .frameCount = 588, .ordinal = 1},
                                               {.startFrame = 588, .frameCount = 87612, .ordinal = 3}});
      auto const appliedRes = ScanApplyOperation{library, std::move(*planRes), {}, {}}.run();
      REQUIRE(appliedRes);
      CHECK(appliedRes->failureCount == 0);
      CHECK(appliedRes->insertedIds.empty());
      CHECK(appliedRes->mutatedIds.empty());
      auto transaction = library.readTransaction();
      CHECK(library.libraryRevision(transaction) == revision);
      auto const reader = library.tracks().reader(transaction);
      auto const optFirst = reader.get(ids[0]);
      auto const optSecond = reader.get(ids[1]);
      REQUIRE(optFirst);
      REQUIRE(optSecond);
      CHECK(optFirst->property().codec() == hints.firstCodec);
      CHECK(optSecond->property().codec() == hints.secondCodec);
      CHECK(optFirst->property().segment() == library::TrackSegment{.startFrame = 0, .frameCount = 588, .ordinal = 1});
      CHECK(optSecond->property().segment() ==
            library::TrackSegment{.startFrame = 588, .frameCount = 87612, .ordinal = 3});
      auto const optStored = library.manifest().reader(transaction).get("disc.flac");
      REQUIRE(optStored);
      CHECK(std::vector<TrackId>{optStored->segmentTrackIds().begin(), optStored->segmentTrackIds().end()} == ids);
      CHECK(optStored->totalFrames() == 88200);
    }
  }

  TEST_CASE("CUE carrier facts - a carrier without an exact physical frame count stays undetermined",
            "[runtime][integration][cue-carrier-facts]")
  {
    auto const temp = ao::test::TempDir{};
    auto const root = temp.path() / "music";
    std::filesystem::create_directories(root);
    // STREAMINFO may leave the sample count unknown; the stored total must not stand in for it.
    writeScanFlacMetadataFixture(root / "disc.flac", {"TITLE=Unknown length"}, 0);
    writeFactsSheet(root / "disc.cue");
    auto library = library::test::makeTestMusicLibrary(root, temp.path() / "db");
    auto const document = cueDocument(cueRecord(1, 0, 588, 88200, 44100, "      codec: FLAC\n") +
                                      cueRecord(3, 588, 87612, 88200, 44100, "      codec: FLAC\n"));
    REQUIRE(importCue(library, temp.path() / "restore.yaml", document));
    auto const revision = library.libraryRevision(library.readTransaction());
    auto planRes = LibraryScan{library}.buildPlan();
    REQUIRE(planRes);
    REQUIRE(planRes->size() == 1);
    auto const& item = planRes->items().front();
    CHECK(item.classification == ScanClassification::Unchanged);
    CHECK(item.cueDiagnostic == ScanCueDiagnostic::UnsupportedCarrier);
    CHECK(isScanNotice(item));
    CHECK_FALSE(item.hasExactMediaFacts);
    CHECK(item.totalFrames == 0);
    CHECK(item.sampleRate == 44100);
    CHECK(item.cueLayout.empty());
    auto const appliedRes = ScanApplyOperation{library, std::move(*planRes), {}, {}}.run();
    REQUIRE(appliedRes);
    CHECK(appliedRes->failureCount == 0);
    CHECK(appliedRes->insertedIds.empty());
    CHECK(appliedRes->mutatedIds.empty());
    auto transaction = library.readTransaction();
    CHECK(library.libraryRevision(transaction) == revision);
    auto const optStored = library.manifest().reader(transaction).get("disc.flac");
    REQUIRE(optStored);
    CHECK(optStored->segmentCount() == 2);
    CHECK(optStored->totalFrames() == 88200);
    auto const reader = library.tracks().reader(transaction);
    REQUIRE(reader.entryCount() == 2);
    auto const optFirst = reader.get(optStored->segmentTrackIds().front());
    auto const optSecond = reader.get(optStored->segmentTrackIds().back());
    REQUIRE(optFirst);
    REQUIRE(optSecond);
    CHECK(optFirst->property().codec() == AudioCodec::Flac);
    CHECK(optSecond->property().codec() == AudioCodec::Flac);
    CHECK(optFirst->property().segment() == library::TrackSegment{.startFrame = 0, .frameCount = 588, .ordinal = 1});
    CHECK(optSecond->property().segment() ==
          library::TrackSegment{.startFrame = 588, .frameCount = 87612, .ordinal = 3});
  }
} // namespace ao::rt::test
