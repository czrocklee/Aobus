// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/ScanApplyOperation.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/library/ScanApplyTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWriter.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/utility/Xxh3.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    struct FailureLog final
    {
      std::int32_t count = 0;
      std::string stage;

      compat::MoveOnlyFunction<void(ScanFailure const&)> callback()
      {
        return [this](ScanFailure const& failure)
        {
          ++count;
          stage = failure.stage;
        };
      }
    };

    void removeTrack(library::MusicLibrary& library, TrackId const trackId)
    {
      auto transaction = library::test::writeTransaction(library);
      auto res = transaction.apply(
        [trackId](library::LibraryWrite& write) -> Result<>
        {
          auto removeRes = write.tracks().remove(trackId);
          REQUIRE(removeRes);
          REQUIRE(*removeRes);
          return {};
        });
      REQUIRE(res);
      REQUIRE(transaction.commit());
    }
  } // namespace

  TEST_CASE("ScanApplyOperation - changed file merges technical facts into concurrently curated metadata",
            "[runtime][unit][library-scan][admission][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = temp.path() / "music";
    std::filesystem::create_directories(musicRoot);
    auto const target = musicRoot / "song.flac";
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), target);
    auto library = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");
    auto const trackId = importOne(library);
    replaceFile(target, audio::test::requireAudioFixture("hires.flac"));
    auto plan = LibraryScan{library}.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::Changed) == 1);
    auto failures = FailureLog{};
    auto operation = ScanApplyOperation{library, std::move(plan), {}, failures.callback()};
    requirePrepared(operation);

    library::test::updateTrackSpec(library,
                                   trackId,
                                   [](library::test::TrackSpec& spec)
                                   {
                                     spec.title = "Curated while scanning";
                                     spec.tags = {"favorite"};
                                   });

    requireRevalidation(operation, 0, 0);

    auto res = operation.run();

    REQUIRE(res);
    CHECK(res->mutatedIds == std::vector{trackId});
    CHECK(res->failureCount == 0);
    CHECK(failures.count == 0);
    auto transaction = library.readTransaction();
    auto const optTrack = library.tracks().reader(transaction).get(trackId);
    REQUIRE(optTrack);
    auto const spec = library::test::trackSpecFromView(library, *optTrack);
    CHECK(spec.title == "Curated while scanning");
    CHECK(spec.tags == std::vector<std::string>{"favorite"});
    CHECK(optTrack->property().sampleRate() == 96000);
    CHECK(optTrack->property().bitDepth() == 24);
  }

  TEST_CASE("ScanApplyOperation - deleting a changed Track during preparation skips it without aborting",
            "[runtime][unit][library-scan][admission][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = temp.path() / "music";
    std::filesystem::create_directories(musicRoot);
    auto const target = musicRoot / "song.flac";
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), target);
    auto library = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");
    auto const trackId = importOne(library);
    replaceFile(target, audio::test::requireAudioFixture("hires.flac"));
    auto plan = LibraryScan{library}.buildPlan().value();
    auto failures = FailureLog{};
    auto operation = ScanApplyOperation{library, std::move(plan), {}, failures.callback()};
    requirePrepared(operation);

    removeTrack(library, trackId);
    requireRevalidation(operation, 0, 0);

    auto res = operation.run();

    REQUIRE(res);
    CHECK(res->mutatedIds.empty());
    CHECK(res->staleCount == 1);
    CHECK(res->failureCount == 0);
    CHECK(failures.count == 0);
    auto transaction = library.readTransaction();
    CHECK_FALSE(library.tracks().reader(transaction).get(trackId));
    CHECK_FALSE(library.manifest().reader(transaction).get("song.flac"));
  }

  TEST_CASE("ScanApplyOperation - a replacement at a planned missing URI is not marked missing",
            "[runtime][unit][library-scan][admission][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = temp.path() / "music";
    std::filesystem::create_directories(musicRoot);
    auto const target = musicRoot / "song.flac";
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), target);
    auto library = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");
    auto const originalTrackId = importOne(library);
    std::filesystem::remove(target);
    auto plan = LibraryScan{library}.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::Missing) == 1);
    auto failures = FailureLog{};
    auto operation = ScanApplyOperation{library, std::move(plan), {}, failures.callback()};
    requirePrepared(operation);

    removeTrack(library, originalTrackId);
    auto replacement = library::test::makeEmptyTrackSpec("song.flac");
    replacement.title = "Replacement";
    auto const replacementTrackId = library::test::addTrack(library, replacement);
    requireRevalidation(operation, 0, 0);

    auto res = operation.run();

    REQUIRE(res);
    CHECK(res->missingCount == 0);
    CHECK(res->staleCount == 1);
    CHECK(res->failureCount == 0);
    CHECK(failures.count == 0);
    auto transaction = library.readTransaction();
    auto const optManifest = library.manifest().reader(transaction).get("song.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->trackId() == replacementTrackId);
    CHECK(optManifest->status() == library::FileStatus::Available);
  }

  TEST_CASE("ScanApplyOperation - independently updated identity does not stale a missing item",
            "[runtime][unit][library-scan][admission][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = temp.path() / "music";
    std::filesystem::create_directories(musicRoot);
    auto const target = musicRoot / "song.flac";
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), target);
    auto library = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");
    auto const trackId = importOne(library);
    std::filesystem::remove(target);
    auto plan = LibraryScan{library}.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::Missing) == 1);
    auto failures = FailureLog{};
    auto operation = ScanApplyOperation{library, std::move(plan), {}, failures.callback()};
    requirePrepared(operation);
    auto const replacementSignature = utility::xxh3Hash128("identity completed after scan planning");

    {
      auto transaction = library::test::writeTransaction(library);
      REQUIRE(transaction.apply(
        [&](library::LibraryWrite& write) -> Result<>
        {
          auto writer = write.tracks();
          auto const optManifest = writer.manifest("song.flac");
          REQUIRE(optManifest);
          auto builder = library::FileManifestBuilder::fromView(*optManifest);
          builder.audioPayloadLength(42).audioSignature(replacementSignature);
          return writer.updateManifest(trackId, builder);
        }));
      REQUIRE(transaction.commit());
    }

    requireRevalidation(operation, 0, 0);

    auto res = operation.run();

    REQUIRE(res);
    CHECK(res->missingCount == 1);
    CHECK(res->staleCount == 0);
    CHECK(res->failureCount == 0);
    CHECK(failures.count == 0);
    auto transaction = library.readTransaction();
    auto const optManifest = library.manifest().reader(transaction).get("song.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->status() == library::FileStatus::Missing);
    CHECK(optManifest->audioPayloadLength() == 42);
    CHECK(optManifest->audioSignature() == replacementSignature);
  }

  TEST_CASE("ScanApplyOperation - an occupied moved destination aborts all co-planned writes",
            "[runtime][unit][library-scan][admission][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = temp.path() / "music";
    std::filesystem::create_directories(musicRoot);
    auto const original = musicRoot / "song.flac";
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), original);
    auto library = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");
    auto const originalTrackId = importOne(library);
    auto const moved = musicRoot / "renamed.flac";
    std::filesystem::rename(original, moved);
    std::filesystem::copy_file(audio::test::requireAudioFixture("hires.flac"), musicRoot / "peer.flac");
    auto plan = LibraryScan{library}.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::Moved) == 1);
    REQUIRE(plan.count(ScanClassification::New) == 1);
    auto failures = FailureLog{};
    auto operation = ScanApplyOperation{library, std::move(plan), {}, failures.callback()};
    requirePrepared(operation);

    auto occupier = library::test::makeEmptyTrackSpec("renamed.flac");
    occupier.title = "Concurrent destination";
    auto const occupyingTrackId = library::test::addTrack(library, occupier);
    requireRevalidation(operation, 0, 0);

    auto res = operation.run();

    REQUIRE(res);
    CHECK(res->insertedIds.empty());
    CHECK(res->relinkedIds.empty());
    CHECK(res->staleCount == 0);
    CHECK(res->failureCount == 1);
    CHECK(failures.count == 1);
    auto transaction = library.readTransaction();
    auto trackReader = library.tracks().reader(transaction);
    auto const optOriginal = trackReader.get(originalTrackId);
    REQUIRE(optOriginal);
    CHECK(optOriginal->property().uri() == "song.flac");
    CHECK(trackReader.get(occupyingTrackId));
    CHECK(library.manifest().reader(transaction).get("song.flac"));
    CHECK(library.manifest().reader(transaction).get("renamed.flac"));
    CHECK_FALSE(library.manifest().reader(transaction).get("peer.flac"));
  }

  TEST_CASE("ScanApplyOperation - a deleted moved source aborts all co-planned writes",
            "[runtime][unit][library-scan][admission][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = temp.path() / "music";
    std::filesystem::create_directories(musicRoot);
    auto const original = musicRoot / "song.flac";
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), original);
    auto library = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");
    auto const originalTrackId = importOne(library);
    std::filesystem::rename(original, musicRoot / "renamed.flac");
    std::filesystem::copy_file(audio::test::requireAudioFixture("hires.flac"), musicRoot / "peer.flac");
    auto plan = LibraryScan{library}.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::Moved) == 1);
    REQUIRE(plan.count(ScanClassification::New) == 1);
    auto failures = FailureLog{};
    auto operation = ScanApplyOperation{library, std::move(plan), {}, failures.callback()};
    requirePrepared(operation);

    removeTrack(library, originalTrackId);
    requireRevalidation(operation, 0, 0);

    auto res = operation.run();

    REQUIRE(res);
    CHECK(res->insertedIds.empty());
    CHECK(res->relinkedIds.empty());
    CHECK(res->staleCount == 0);
    CHECK(res->failureCount == 1);
    CHECK(failures.count == 1);
    auto transaction = library.readTransaction();
    CHECK_FALSE(library.tracks().reader(transaction).get(originalTrackId));
    auto manifestReader = library.manifest().reader(transaction);
    CHECK_FALSE(manifestReader.get("song.flac"));
    CHECK_FALSE(manifestReader.get("renamed.flac"));
    CHECK_FALSE(manifestReader.get("peer.flac"));
  }
} // namespace ao::rt::test
