// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/library/ScanApplyOperation.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include <ao/async/OperationCancelled.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/ScanPlan.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

    auto manifestResult = libraryFixture.library().manifest().reader(transaction).get("song.flac");
    REQUIRE(manifestResult);
    CHECK(manifestResult->trackId() == result->insertedIds[0]);
    CHECK(library::hasAudioIdentity(manifestResult->audioPayloadLength(), manifestResult->audioSignature()));
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
    auto manifestResult = libraryFixture.library().manifest().reader(transaction).get("song.flac");
    REQUIRE(manifestResult);
    CHECK_FALSE(library::hasAudioIdentity(manifestResult->audioPayloadLength(), manifestResult->audioSignature()));
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
} // namespace ao::rt::test
