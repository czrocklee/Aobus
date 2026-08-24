// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/LibraryScan.h>

#include "runtime/library/ScanApplyOperation.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/library/ScanPlan.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    void copyBasicAudioFixture(MusicLibraryFixture& libraryFixture, std::string_view uri = "song.flac")
    {
      auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
      std::filesystem::copy_file(sourceFile, libraryFixture.root() / std::filesystem::path{uri});
    }

    void setManifestStatus(MusicLibraryFixture& libraryFixture,
                           std::string_view const uri,
                           library::FileStatus const status)
    {
      auto transaction = library::test::writeTransaction(libraryFixture.library());
      REQUIRE(transaction.apply(
        [&](library::LibraryWrite& write) -> Result<>
        {
          auto writer = write.tracks();
          auto const optManifest = writer.manifest(uri);
          REQUIRE(optManifest);
          auto builder = library::FileManifestBuilder::fromView(*optManifest);
          builder.status(status);
          return writer.updateManifest(optManifest->trackId(), builder);
        }));
      REQUIRE(transaction.commit());
    }

    std::uint64_t libraryRevision(MusicLibraryFixture& libraryFixture)
    {
      auto transaction = libraryFixture.library().readTransaction();
      return libraryFixture.library().libraryRevision(transaction);
    }
  } // namespace

  TEST_CASE("LibraryScan - buildPlan reports new audio files", "[runtime][unit][library][scan]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    copyBasicAudioFixture(libraryFixture);

    auto service = LibraryScan{libraryFixture.library()};
    auto progressPaths = std::vector<std::filesystem::path>{};
    auto result =
      service.buildPlan([&progressPaths](std::filesystem::path const& path) { progressPaths.push_back(path); });

    REQUIRE(result);
    REQUIRE(result->size() == 1);
    CHECK(result->items()[0].uri == "song.flac");
    CHECK(result->items()[0].classification == ScanClassification::New);
    CHECK(std::ranges::any_of(
      progressPaths, [](std::filesystem::path const& path) { return path.filename() == "song.flac"; }));
  }

  TEST_CASE("LibraryScan - buildPlan cancellation before start performs no filesystem work",
            "[runtime][unit][library-scan][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    copyBasicAudioFixture(libraryFixture);
    auto stopSource = std::stop_source{};
    stopSource.request_stop();
    bool reportedProgress = false;

    REQUIRE_THROWS_AS(
      LibraryScan{libraryFixture.library()}.buildPlan(
        [&reportedProgress](std::filesystem::path const&) { reportedProgress = true; }, stopSource.get_token()),
      async::OperationCancelled);
    CHECK_FALSE(reportedProgress);
  }

  TEST_CASE("LibraryScan - buildPlan cancellation from progress stops before the next entry",
            "[runtime][unit][library-scan][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    copyBasicAudioFixture(libraryFixture, "first.flac");
    copyBasicAudioFixture(libraryFixture, "second.flac");
    auto stopSource = std::stop_source{};
    std::size_t progressCount = 0;

    REQUIRE_THROWS_AS(LibraryScan{libraryFixture.library()}.buildPlan(
                        [&stopSource, &progressCount](std::filesystem::path const&)
                        {
                          ++progressCount;
                          stopSource.request_stop();
                        },
                        stopSource.get_token()),
                      async::OperationCancelled);
    CHECK(progressCount == 1);
  }

  TEST_CASE("LibraryScan - applyPlan imports new tracks", "[runtime][unit][library][scan]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    copyBasicAudioFixture(libraryFixture);

    auto service = LibraryScan{libraryFixture.library()};
    auto plan = service.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::New) == 1);

    auto operation = ScanApplyOperation{libraryFixture.library(), std::move(plan), {}, {}};
    auto result = operation.run();

    REQUIRE(result);
    REQUIRE(result->insertedIds.size() == 1);
    CHECK(result->failureCount == 0);
    CHECK_FALSE(operation.cancelled());

    auto transaction = libraryFixture.library().readTransaction();
    auto trackReader = libraryFixture.library().tracks().reader(transaction);
    auto optTrack = trackReader.get(result->insertedIds[0]);
    REQUIRE(optTrack);
    CHECK(optTrack->metadata().title() == "Test Title");

    auto optManifest = libraryFixture.library().manifest().reader(transaction).get("song.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->trackId() == result->insertedIds[0]);
    CHECK(library::hasAudioIdentity(optManifest->audioPayloadLength(), optManifest->audioSignature()));
  }

  TEST_CASE("LibraryScan - applyPlan can defer new audio identity", "[runtime][unit][library][scan]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    copyBasicAudioFixture(libraryFixture);

    auto service = LibraryScan{libraryFixture.library()};
    auto plan = service.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::New) == 1);

    auto result = ScanApplyOperation{libraryFixture.library(),
                                     std::move(plan),
                                     {},
                                     {},
                                     ScanApplyOptions{.audioIdentityPolicy = AudioIdentityPolicy::DeferNew}}
                    .run();

    REQUIRE(result);
    REQUIRE(result->insertedIds.size() == 1);
    CHECK(result->failureCount == 0);

    auto transaction = libraryFixture.library().readTransaction();
    auto optManifest = libraryFixture.library().manifest().reader(transaction).get("song.flac");
    REQUIRE(optManifest);
    CHECK_FALSE(library::hasAudioIdentity(optManifest->audioPayloadLength(), optManifest->audioSignature()));
  }

  TEST_CASE("LibraryScan - applyPlan honors cancellation", "[runtime][unit][library][scan]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    copyBasicAudioFixture(libraryFixture);

    auto service = LibraryScan{libraryFixture.library()};
    auto plan = service.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::New) == 1);

    auto stopSource = std::stop_source{};
    stopSource.request_stop();
    auto operation = ScanApplyOperation{libraryFixture.library(), std::move(plan), {}, {}, {}};
    REQUIRE_THROWS_AS(operation.run(stopSource.get_token()), async::OperationCancelled);
    CHECK(operation.cancelled());

    auto transaction = libraryFixture.library().readTransaction();
    auto trackReader = libraryFixture.library().tracks().reader(transaction);
    auto manifestReader = libraryFixture.library().manifest().reader(transaction);
    CHECK(trackReader.begin() == trackReader.end());
    CHECK(manifestReader.begin() == manifestReader.end());
  }

  TEST_CASE("LibraryScan - a present file restores a missing manifest with unchanged file facts",
            "[runtime][regression][library-scan]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    copyBasicAudioFixture(libraryFixture);

    auto initialPlan = LibraryScan{libraryFixture.library()}.buildPlan().value();
    auto initialRes = ScanApplyOperation{libraryFixture.library(), std::move(initialPlan), {}, {}}.run();
    REQUIRE(initialRes);
    REQUIRE(initialRes->insertedIds.size() == 1);
    auto const trackId = initialRes->insertedIds.front();
    setManifestStatus(libraryFixture, "song.flac", library::FileStatus::Missing);

    auto recoveryPlan = LibraryScan{libraryFixture.library()}.buildPlan().value();
    REQUIRE(recoveryPlan.size() == 1);
    CHECK(recoveryPlan.items().front().classification == ScanClassification::Changed);
    REQUIRE(recoveryPlan.items().front().optManifestEvidence);
    CHECK(recoveryPlan.items().front().optManifestEvidence->status == library::FileStatus::Missing);

    auto recoveryRes = ScanApplyOperation{libraryFixture.library(), std::move(recoveryPlan), {}, {}}.run();
    REQUIRE(recoveryRes);
    REQUIRE(recoveryRes->mutatedIds.size() == 1);
    CHECK(recoveryRes->mutatedIds.front() == trackId);

    auto transaction = libraryFixture.library().readTransaction();
    auto const optManifest = libraryFixture.library().manifest().reader(transaction).get("song.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->status() == library::FileStatus::Available);
  }

  TEST_CASE("LibraryScan - an already missing file is reported without advancing the revision",
            "[runtime][regression][library-scan]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    copyBasicAudioFixture(libraryFixture);

    auto initialPlan = LibraryScan{libraryFixture.library()}.buildPlan().value();
    auto initialRes = ScanApplyOperation{libraryFixture.library(), std::move(initialPlan), {}, {}}.run();
    REQUIRE(initialRes);
    std::filesystem::remove(libraryFixture.root() / "song.flac");

    auto missingPlan = LibraryScan{libraryFixture.library()}.buildPlan().value();
    auto missingRes = ScanApplyOperation{libraryFixture.library(), std::move(missingPlan), {}, {}}.run();
    REQUIRE(missingRes);
    CHECK(missingRes->missingCount == 1);
    auto const missingRevision = libraryRevision(libraryFixture);

    auto repeatedPlan = LibraryScan{libraryFixture.library()}.buildPlan().value();
    REQUIRE(repeatedPlan.size() == 1);
    CHECK(repeatedPlan.items().front().classification == ScanClassification::Missing);
    REQUIRE(repeatedPlan.items().front().optManifestEvidence);
    CHECK(repeatedPlan.items().front().optManifestEvidence->status == library::FileStatus::Missing);
    auto repeatedRes = ScanApplyOperation{libraryFixture.library(), std::move(repeatedPlan), {}, {}}.run();

    REQUIRE(repeatedRes);
    CHECK(repeatedRes->missingCount == 1);
    CHECK(repeatedRes->libraryRevision == 0);
    CHECK(libraryRevision(libraryFixture) == missingRevision);
  }
} // namespace ao::rt::test
