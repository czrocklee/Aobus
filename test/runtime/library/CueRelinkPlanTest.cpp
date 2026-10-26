// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/ScanApplyOperation.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include "test/runtime/library/ScanApplyTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/FileTimestamp.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/ScanPlan.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    constexpr auto kSheet = "FILE ignored.wav WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"
                            "TRACK 02 AUDIO\nINDEX 01 00:00:01\n";
    constexpr auto kDifferentSheet = "FILE ignored.wav WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"
                                     "TRACK 02 AUDIO\nINDEX 01 00:00:02\n";

    void writeCue(std::filesystem::path const& path, std::string_view text)
    {
      auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
      stream << text;
      REQUIRE(stream.good());
    }

    ScanPlan planFor(library::MusicLibrary const& library)
    {
      auto res = LibraryScan{library}.buildPlan();
      REQUIRE(res);
      return std::move(*res);
    }

    ScanApplyResult scan(library::MusicLibrary& library)
    {
      auto res = ScanApplyOperation{library, planFor(library), {}, {}}.run();
      REQUIRE(res);
      REQUIRE(res->failureCount == 0);
      return std::move(*res);
    }

    ScanItem const& requireItem(ScanPlan const& plan, std::string_view uri)
    {
      auto const items = plan.items();
      auto const found = std::ranges::find(items, uri, &ScanItem::uri);
      REQUIRE(found != items.end());
      return *found;
    }
  } // namespace

  TEST_CASE("CUE relink plan - rejects known carrier facts for rejected and ambiguous destinations",
            "[runtime][integration][cue-relink-plan]")
  {
    auto fixture = MusicLibraryFixture{};
    auto& library = fixture.library();
    auto const& root = fixture.root();
    audio::test::installAudioFixture(root, "basic_metadata.wav", "old.wav");
    writeCue(root / "old.cue", kSheet);
    auto const imported = scan(library);
    REQUIRE(imported.insertedIds.size() == 2);
    bool isAmbiguous = false;

    SECTION("automatic rejection retains inspected facts")
    {
    }

    SECTION("multiple missing sources leave the incompatible destination New")
    {
      std::filesystem::copy_file(root / "old.wav", root / "duplicate.wav");
      REQUIRE(scan(library).insertedIds.size() == 1);
      std::filesystem::remove(root / "duplicate.wav");
      isAmbiguous = true;
    }

    std::filesystem::remove(root / "old.cue");
    std::filesystem::rename(root / "old.wav", root / "new.wav");

    {
      // WAV identity covers PCM, not these sample-rate and byte-rate fields.
      auto stream = std::fstream{root / "new.wav", std::ios::binary | std::ios::in | std::ios::out};
      auto const rateHeader = std::to_array<unsigned char>({0x80, 0xbb, 0, 0, 0, 0xee, 2, 0});
      stream.seekp(24);
      stream.write(reinterpret_cast<char const*>(rateHeader.data()), rateHeader.size());
      REQUIRE(stream.good());
    }

    auto plan = planFor(library);
    auto const& source = requireItem(plan, "old.wav");
    auto const& destination = requireItem(plan, "new.wav");
    REQUIRE(source.optManifestEvidence);
    CHECK(destination.classification == (isAmbiguous ? ScanClassification::New : ScanClassification::Error));
    CHECK(destination.cueDiagnostic ==
          (isAmbiguous ? ScanCueDiagnostic::None : ScanCueDiagnostic::CarrierFactsMismatch));
    CHECK(source.audioPayloadLength == destination.audioPayloadLength);
    CHECK(source.audioSignature == destination.audioSignature);
    CHECK(source.optManifestEvidence->sampleRate == 44100);
    CHECK(destination.hasExactMediaFacts);
    CHECK(destination.sampleRate == 48000);
    CHECK(destination.totalFrames == 44100);
    CHECK_FALSE(canRelinkItems(source, destination));
    auto rejectedRes = std::move(plan).makeRelinkPlan("old.wav", "new.wav");
    REQUIRE_FALSE(rejectedRes);
    CHECK(rejectedRes.error().code == Error::Code::InvalidInput);
    auto transaction = library.readTransaction();
    CHECK_FALSE(library.manifest().reader(transaction).get("new.wav"));
    auto optStored = library.manifest().reader(transaction).get("old.wav");
    REQUIRE(optStored);
    auto const optMember = library.tracks().reader(transaction).get(imported.insertedIds.front());
    REQUIRE(optMember);
    CHECK(optMember->property().sampleRate() == 44100);
    CHECK(std::vector<TrackId>{optStored->segmentTrackIds().begin(), optStored->segmentTrackIds().end()} ==
          imported.insertedIds);
  }

  TEST_CASE("CUE relink plan - compatible explicit moves classify the selected stored form",
            "[runtime][integration][cue-relink-plan]")
  {
    auto fixture = MusicLibraryFixture{};
    auto& library = fixture.library();
    auto const& root = fixture.root();
    bool isCarrier = true;
    auto destinationSheet = std::string_view{kSheet};
    auto expected = ScanCueDiagnostic::None;
    bool hasCompetingSheet = false;

    SECTION("matching carrier")
    {
    }

    SECTION("whole file without a dividing sheet")
    {
      isCarrier = false;
      destinationSheet = {};
    }

    SECTION("carrier without a dividing sheet")
    {
      destinationSheet = {};
      expected = ScanCueDiagnostic::SheetMissingForCarrier;
    }

    SECTION("carrier with a different valid layout")
    {
      destinationSheet = kDifferentSheet;
      expected = ScanCueDiagnostic::LayoutMismatch;
    }

    SECTION("whole file with a valid dividing sheet")
    {
      isCarrier = false;
      expected = ScanCueDiagnostic::WholeFileAlreadyImported;
    }

    SECTION("malformed intrinsic binding survives source classification")
    {
      destinationSheet = "not a CUE sheet";
      expected = ScanCueDiagnostic::SheetUnreadableOrMalformed;
    }

    SECTION("ambiguous intrinsic binding survives source classification")
    {
      hasCompetingSheet = true;
      expected = ScanCueDiagnostic::AmbiguousSheets;
    }

    audio::test::installAudioFixture(root, "basic_metadata.wav", "old.wav");
    std::filesystem::copy_file(root / "old.wav", root / "duplicate.wav");

    if (isCarrier)
    {
      writeCue(root / "old.cue", kSheet);
    }

    auto const imported = scan(library);
    auto sourceIds = imported.insertedIds;
    std::erase_if(sourceIds, [&](TrackId id) { return storedTrackSpec(library, id).uri != "old.wav"; });
    REQUIRE(sourceIds.size() == (isCarrier ? 2 : 1));
    fixture.updateTrack(sourceIds.front(),
                        [](library::test::TrackSpec& spec)
                        {
                          spec.title = "Curated title";
                          spec.artist = "Curated artist";
                        });
    auto const before = storedTrackSpec(library, sourceIds.front());
    std::filesystem::remove(root / "duplicate.wav");
    std::filesystem::remove(root / "old.cue");
    std::filesystem::rename(root / "old.wav", root / "new.wav");

    if (!destinationSheet.empty())
    {
      writeCue(root / "new.cue", destinationSheet);
    }

    if (hasCompetingSheet)
    {
      if (std::filesystem::exists(root / "new.CUE"))
      {
        SKIP("Competing case-variant sheets require a case-sensitive filesystem");
      }

      writeCue(root / "new.CUE", kSheet);
    }

    auto plan = planFor(library);
    CHECK(plan.count(ScanClassification::Moved) == 0);
    auto const evidence = requireItem(plan, "new.wav").cueEvidence;
    auto relinkRes = std::move(plan).makeRelinkPlan("old.wav", "new.wav");
    REQUIRE(relinkRes);
    REQUIRE(relinkRes->size() == 1);
    auto const& moved = relinkRes->items().front();
    CHECK(moved.classification == ScanClassification::Moved);
    CHECK(moved.cueDiagnostic == expected);
    CHECK(moved.cueEvidence == evidence);
    CHECK(moved.errorMessage.empty() == (expected == ScanCueDiagnostic::None));
    // The relink commits, so its binding diagnostic is a notice rather than a failure.
    CHECK(isScanNotice(moved) == (expected != ScanCueDiagnostic::None));
    auto appliedRes = ScanApplyOperation{library, std::move(*relinkRes), {}, {}}.run();
    REQUIRE(appliedRes);
    CHECK(appliedRes->relinkedIds == sourceIds);
    CHECK(appliedRes->relinkedFileCount == 1);
    CHECK(appliedRes->failureCount == 0);
    auto const after = storedTrackSpec(library, sourceIds.front());
    CHECK(after.uri == "new.wav");
    CHECK(after.title == before.title);
    CHECK(after.artist == before.artist);
    auto transaction = library.readTransaction();
    CHECK_FALSE(library.manifest().reader(transaction).get("old.wav"));
    auto optStored = library.manifest().reader(transaction).get("new.wav");
    REQUIRE(optStored);
    CHECK(optStored->segmentCount() == (isCarrier ? 2 : 0));

    if (isCarrier)
    {
      CHECK(optStored->totalFrames() == 44100);
      auto optFirst = library.tracks().reader(transaction).get(sourceIds.front());
      REQUIRE(optFirst);
      CHECK(optFirst->property().sampleRate() == 44100);
      REQUIRE(optFirst->property().segment());
      CHECK(optFirst->property().segment()->startFrame == 0);
      CHECK(optFirst->property().segment()->frameCount == 588);
      auto optSecond = library.tracks().reader(transaction).get(sourceIds.back());
      REQUIRE(optSecond);
      REQUIRE(optSecond->property().segment());
      CHECK(optSecond->property().segment()->ordinal == 2);
      CHECK(optSecond->property().segment()->startFrame == 588);
      CHECK(optSecond->property().segment()->frameCount == 43512);
    }
  }

  TEST_CASE("CUE relink plan - Error admission requires a typed binding failure and observed carrier facts",
            "[runtime][unit][cue-relink-plan]")
  {
    auto source = ScanItem{.uri = "old.wav",
                           .classification = ScanClassification::Missing,
                           .audioPayloadLength = 17,
                           .audioSignature = {},
                           .trackId = TrackId{1}};
    source.audioSignature.bytes.front() = std::byte{1};
    auto destination = ScanItem{.uri = "new.wav",
                                .fullPath = "/new.wav",
                                .classification = ScanClassification::Error,
                                .optMtime = FileTimestamp{},
                                .audioPayloadLength = source.audioPayloadLength,
                                .audioSignature = source.audioSignature};
    CHECK_FALSE(isRelinkDestination(destination));
    CHECK_FALSE(canRelinkItems(source, destination));

    for (auto diagnostic : {ScanCueDiagnostic::InvalidEncoding,
                            ScanCueDiagnostic::SheetUnreadableOrMalformed,
                            ScanCueDiagnostic::AmbiguousSheets,
                            ScanCueDiagnostic::UnsupportedCarrier,
                            ScanCueDiagnostic::LayoutMismatch})
    {
      destination.cueDiagnostic = diagnostic;
      CHECK(isRelinkDestination(destination));
      CHECK(canRelinkItems(source, destination));
    }

    destination.cueDiagnostic = ScanCueDiagnostic::CarrierFactsMismatch;
    CHECK_FALSE(isRelinkDestination(destination));
    destination.cueDiagnostic = ScanCueDiagnostic::SheetUnreadableOrMalformed;
    source.optManifestEvidence =
      ScanManifestEvidence{.totalFrames = 44100, .sampleRate = 44100, .members = {{.trackId = TrackId{1}}}};
    destination.totalFrames = 44100;
    destination.sampleRate = 44100;
    CHECK_FALSE(canRelinkItems(source, destination));
    destination.hasExactMediaFacts = true;
    CHECK(canRelinkItems(source, destination));
    destination.totalFrames = 44101;
    CHECK_FALSE(canRelinkItems(source, destination));
    destination.totalFrames = 44100;
    destination.sampleRate = 48000;
    CHECK_FALSE(canRelinkItems(source, destination));
    destination.classification = ScanClassification::New;
    CHECK_FALSE(canRelinkItems(source, destination));
    destination.sampleRate = 44100;
    CHECK(canRelinkItems(source, destination));
  }

  TEST_CASE("CUE relink plan - unsupported external binding does not prevent a valid whole-file move",
            "[runtime][integration][cue-relink-plan]")
  {
    auto fixture = MusicLibraryFixture{};
    auto& library = fixture.library();
    auto const& root = fixture.root();
    audio::test::installAudioFixture(root, "basic_metadata.opus", "old.opus");
    std::filesystem::copy_file(root / "old.opus", root / "duplicate.opus");
    auto const imported = scan(library);
    REQUIRE(imported.insertedIds.size() == 2);
    auto sourceIds = imported.insertedIds;
    std::erase_if(sourceIds, [&](TrackId id) { return storedTrackSpec(library, id).uri != "old.opus"; });
    REQUIRE(sourceIds.size() == 1);
    std::filesystem::remove(root / "duplicate.opus");
    std::filesystem::rename(root / "old.opus", root / "new.opus");
    writeCue(root / "new.cue", kSheet);
    auto plan = planFor(library);
    CHECK(requireItem(plan, "new.opus").classification == ScanClassification::Error);
    auto relinkRes = std::move(plan).makeRelinkPlan("old.opus", "new.opus");
    REQUIRE(relinkRes);
    CHECK(relinkRes->items().front().cueDiagnostic == ScanCueDiagnostic::UnsupportedCarrier);
    auto appliedRes = ScanApplyOperation{library, std::move(*relinkRes), {}, {}}.run();
    REQUIRE(appliedRes);
    CHECK(appliedRes->relinkedIds == sourceIds);
    CHECK(appliedRes->relinkedFileCount == 1);
    CHECK(appliedRes->failureCount == 0);
    CHECK(storedTrackSpec(library, sourceIds.front()).uri == "new.opus");
  }
} // namespace ao::rt::test
