// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
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
    void copyBasicAudioFixture(TestMusicLibrary& testLib, std::string_view uri = "song.flac")
    {
      auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
      std::filesystem::copy_file(sourceFile, testLib.root() / std::filesystem::path{uri});
    }
  } // namespace

  TEST_CASE("LibraryScan - buildPlan reports new audio files", "[runtime][unit][library][scan]")
  {
    auto testLib = TestMusicLibrary{};
    copyBasicAudioFixture(testLib);

    auto service = LibraryScan{testLib.library()};
    auto progressPaths = std::vector<std::filesystem::path>{};
    auto result =
      service.buildPlan([&progressPaths](std::filesystem::path const& path) { progressPaths.push_back(path); });

    REQUIRE(result);
    REQUIRE(result->items.size() == 1);
    CHECK(result->items[0].uri == "song.flac");
    CHECK(result->items[0].classification == ScanClassification::New);
    CHECK(std::ranges::any_of(
      progressPaths, [](std::filesystem::path const& path) { return path.filename() == "song.flac"; }));
  }

  TEST_CASE("LibraryScan - applyPlan imports new tracks", "[runtime][unit][library][scan]")
  {
    auto testLib = TestMusicLibrary{};
    copyBasicAudioFixture(testLib);

    auto service = LibraryScan{testLib.library()};
    auto plan = service.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::New) == 1);

    auto result = service.applyPlan(std::move(plan));

    REQUIRE(result);
    REQUIRE(result->processedIds.size() == 1);
    CHECK(result->failureCount == 0);
    CHECK_FALSE(result->cancelled);

    auto transaction = testLib.library().readTransaction();
    auto trackReader = testLib.library().tracks().reader(transaction);
    auto optTrack = trackReader.get(result->processedIds[0]);
    REQUIRE(optTrack);
    CHECK(optTrack->metadata().title() == "Test Title");

    auto manifestResult = testLib.library().manifest().reader(transaction).get("song.flac");
    REQUIRE(manifestResult);
    CHECK(manifestResult->trackId() == result->processedIds[0]);
    CHECK(library::hasAudioIdentity(manifestResult->audioPayloadLength(), manifestResult->audioSignature()));
  }

  TEST_CASE("LibraryScan - applyPlan can defer new audio identity", "[runtime][unit][library][scan]")
  {
    auto testLib = TestMusicLibrary{};
    copyBasicAudioFixture(testLib);

    auto service = LibraryScan{testLib.library()};
    auto plan = service.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::New) == 1);

    auto result =
      service.applyPlan(std::move(plan), ScanApplyOptions{.audioIdentityPolicy = AudioIdentityPolicy::DeferNew});

    REQUIRE(result);
    REQUIRE(result->processedIds.size() == 1);
    CHECK(result->failureCount == 0);

    auto transaction = testLib.library().readTransaction();
    auto manifestResult = testLib.library().manifest().reader(transaction).get("song.flac");
    REQUIRE(manifestResult);
    CHECK_FALSE(library::hasAudioIdentity(manifestResult->audioPayloadLength(), manifestResult->audioSignature()));
  }

  TEST_CASE("LibraryScan - applyPlan honors cancellation", "[runtime][unit][library][scan]")
  {
    auto testLib = TestMusicLibrary{};
    copyBasicAudioFixture(testLib);

    auto service = LibraryScan{testLib.library()};
    auto plan = service.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::New) == 1);

    auto stopSource = std::stop_source{};
    stopSource.request_stop();
    auto result = service.applyPlan(std::move(plan), ScanApplyOptions{}, {}, {}, stopSource.get_token());

    REQUIRE(result);
    CHECK(result->cancelled);
    CHECK(result->processedIds.empty());
    CHECK(result->failureCount == 0);

    auto transaction = testLib.library().readTransaction();
    auto trackReader = testLib.library().tracks().reader(transaction);
    auto manifestReader = testLib.library().manifest().reader(transaction);
    CHECK(trackReader.begin() == trackReader.end());
    CHECK(manifestReader.begin() == manifestReader.end());
  }
} // namespace ao::rt::test
