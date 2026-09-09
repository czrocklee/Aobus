// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/ScanApplyOperation.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/ScanPlan.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <tuple>
#include <utility>

namespace ao::rt::test
{
  namespace
  {
    void replaceFile(std::filesystem::path const& target, std::filesystem::path const& source)
    {
      auto const previousTime = std::filesystem::last_write_time(target);
      std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing);
      std::filesystem::last_write_time(target, previousTime + std::chrono::seconds{10});
    }

    TrackId importOne(library::MusicLibrary& library)
    {
      auto plan = LibraryScan{library}.buildPlan().value();
      auto res = ScanApplyOperation{library, std::move(plan), {}, {}}.run();
      REQUIRE(res);
      REQUIRE(res->insertedIds.size() == 1);
      return res->insertedIds.front();
    }
  } // namespace

  TEST_CASE("ScanApplyOperation - a new file changed after preparation is left for the next scan",
            "[runtime][regression][scan-revalidation]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = temp.path() / "music";
    std::filesystem::create_directories(musicRoot);
    auto const target = musicRoot / "song.flac";
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), target);
    auto library = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");
    auto plan = LibraryScan{library}.buildPlan().value();
    auto operation = ScanApplyOperation{library, std::move(plan), {}, {}};
    REQUIRE(operation.prepare());

    replaceFile(target, audio::test::requireAudioFixture("hires.flac"));
    auto res = operation.run();

    REQUIRE(res);
    CHECK(res->insertedIds.empty());
    CHECK(res->staleCount == 1);
    CHECK(res->failureCount == 0);
    auto transaction = library.readTransaction();
    CHECK_FALSE(library.manifest().reader(transaction).get("song.flac"));
    auto trackReader = library.tracks().reader(transaction);
    CHECK(trackReader.begin() == trackReader.end());
  }

  TEST_CASE("ScanApplyOperation - a changed file changed again after preparation keeps live track data",
            "[runtime][regression][scan-revalidation]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = temp.path() / "music";
    std::filesystem::create_directories(musicRoot);
    auto const target = musicRoot / "song.flac";
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), target);
    auto library = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");
    auto const trackId = importOne(library);
    auto const initialSampleRate = [&]
    {
      auto transaction = library.readTransaction();
      auto const optTrack = library.tracks().reader(transaction).get(trackId);
      REQUIRE(optTrack);
      return optTrack->property().sampleRate();
    }();
    replaceFile(target, audio::test::requireAudioFixture("hires.flac"));
    auto plan = LibraryScan{library}.buildPlan().value();
    REQUIRE(plan.items().front().classification == ScanClassification::Changed);
    auto operation = ScanApplyOperation{library, std::move(plan), {}, {}};
    REQUIRE(operation.prepare());

    replaceFile(target, audio::test::requireAudioFixture("with_cover.flac"));
    auto res = operation.run();

    REQUIRE(res);
    CHECK(res->mutatedIds.empty());
    CHECK(res->staleCount == 1);
    CHECK(res->failureCount == 0);
    auto transaction = library.readTransaction();
    auto const optTrack = library.tracks().reader(transaction).get(trackId);
    REQUIRE(optTrack);
    CHECK(optTrack->property().sampleRate() == initialSampleRate);
  }

  TEST_CASE("ScanApplyOperation - a moved file restamped after preparation is not relinked",
            "[runtime][regression][scan-revalidation]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = temp.path() / "music";
    std::filesystem::create_directories(musicRoot);
    auto const original = musicRoot / "song.flac";
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), original);
    auto library = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");
    auto const trackId = importOne(library);
    auto const moved = musicRoot / "renamed.flac";
    std::filesystem::rename(original, moved);
    auto plan = LibraryScan{library}.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::Moved) == 1);
    auto operation = ScanApplyOperation{library, std::move(plan), {}, {}};
    REQUIRE(operation.prepare());

    auto const preparedTime = std::filesystem::last_write_time(moved);
    std::filesystem::last_write_time(moved, preparedTime + std::chrono::seconds{10});
    auto res = operation.run();

    REQUIRE(res);
    CHECK(res->relinkedIds.empty());
    CHECK(res->staleCount == 0);
    CHECK(res->failureCount == 1);
    auto transaction = library.readTransaction();
    auto const optTrack = library.tracks().reader(transaction).get(trackId);
    REQUIRE(optTrack);
    CHECK(optTrack->property().uri() == "song.flac");
    CHECK(library.manifest().reader(transaction).get("song.flac"));
    CHECK_FALSE(library.manifest().reader(transaction).get("renamed.flac"));
  }

  TEST_CASE("ScanApplyOperation - a missing path that reappears before mutation stays available",
            "[runtime][regression][scan-revalidation]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = temp.path() / "music";
    std::filesystem::create_directories(musicRoot);
    auto const source = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const target = musicRoot / "song.flac";
    std::filesystem::copy_file(source, target);
    auto library = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");
    std::ignore = importOne(library);
    std::filesystem::remove(target);
    auto plan = LibraryScan{library}.buildPlan().value();
    REQUIRE(plan.items().front().classification == ScanClassification::Missing);
    auto operation = ScanApplyOperation{library, std::move(plan), {}, {}};
    REQUIRE(operation.prepare());

    std::filesystem::copy_file(source, target);
    auto res = operation.run();

    REQUIRE(res);
    CHECK(res->missingCount == 0);
    CHECK(res->staleCount == 1);
    CHECK(res->failureCount == 0);
    auto transaction = library.readTransaction();
    auto const optManifest = library.manifest().reader(transaction).get("song.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->status() == library::FileStatus::Available);
  }
} // namespace ao::rt::test
