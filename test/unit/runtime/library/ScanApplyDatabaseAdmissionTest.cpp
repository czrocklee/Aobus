// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/ScanApplyOperation.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
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

#include <chrono>
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

    void replaceFile(std::filesystem::path const& target, std::filesystem::path const& source)
    {
      auto const previousTime = std::filesystem::last_write_time(target);
      std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing);
      std::filesystem::last_write_time(target, previousTime + std::chrono::seconds{10});
    }

    TrackId importOne(library::MusicLibrary& library)
    {
      auto plan = LibraryScan{library}.buildPlan().value();
      auto result = ScanApplyOperation{library, std::move(plan), {}, {}}.run();
      REQUIRE(result);
      REQUIRE(result->insertedIds.size() == 1);
      return result->insertedIds.front();
    }

    void removeTrack(library::MusicLibrary& library, TrackId const trackId)
    {
      auto transaction = library::test::writeTransaction(library);
      auto result = transaction.apply(
        [trackId](library::LibraryWrite& write) -> Result<>
        {
          auto removeRes = write.tracks().remove(trackId);
          REQUIRE(removeRes);
          REQUIRE(*removeRes);
          return {};
        });
      REQUIRE(result);
      REQUIRE(transaction.commit());
    }
  } // namespace

  TEST_CASE("ScanApplyOperation - changed file merges technical facts into concurrently curated metadata",
            "[runtime][regression][scan-admission]")
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
    REQUIRE(plan.count(ScanClassification::Changed) == 1);
    auto failures = FailureLog{};
    auto operation = ScanApplyOperation{library, std::move(plan), {}, failures.callback()};
    REQUIRE(operation.prepare());

    library::test::updateTrackSpec(library,
                                   trackId,
                                   [](library::test::TrackSpec& spec)
                                   {
                                     spec.title = "Curated while scanning";
                                     spec.tags = {"favorite"};
                                   });

    auto result = operation.run();

    REQUIRE(result);
    CHECK(result->mutatedIds == std::vector{trackId});
    CHECK(result->failureCount == 0);
    CHECK(failures.count == 0);
    auto transaction = library.readTransaction();
    auto const optTrack = library.tracks().reader(transaction).get(trackId);
    REQUIRE(optTrack);
    auto const spec = library::test::trackSpecFromView(library, *optTrack);
    CHECK(spec.title == "Curated while scanning");
    CHECK(spec.tags == std::vector<std::string>{"favorite"});
    CHECK(optTrack->property().sampleRate() != initialSampleRate);
  }

  TEST_CASE("ScanApplyOperation - deleting a changed Track during preparation skips it without aborting",
            "[runtime][regression][scan-admission]")
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
    REQUIRE(operation.prepare());

    removeTrack(library, trackId);
    auto result = operation.run();

    REQUIRE(result);
    CHECK(result->mutatedIds.empty());
    CHECK(result->staleCount == 1);
    CHECK(result->failureCount == 0);
    CHECK(failures.count == 0);
    auto transaction = library.readTransaction();
    CHECK_FALSE(library.tracks().reader(transaction).get(trackId));
    CHECK_FALSE(library.manifest().reader(transaction).get("song.flac"));
  }

  TEST_CASE("ScanApplyOperation - a replacement at a planned missing URI is not marked missing",
            "[runtime][regression][scan-admission]")
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
    REQUIRE(operation.prepare());

    removeTrack(library, originalTrackId);
    auto replacement = library::test::makeEmptyTrackSpec("song.flac");
    replacement.title = "Replacement";
    auto const replacementTrackId = library::test::addTrack(library, replacement);
    auto result = operation.run();

    REQUIRE(result);
    CHECK(result->missingCount == 0);
    CHECK(result->staleCount == 1);
    CHECK(result->failureCount == 0);
    CHECK(failures.count == 0);
    auto transaction = library.readTransaction();
    auto const optManifest = library.manifest().reader(transaction).get("song.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->trackId() == replacementTrackId);
    CHECK(optManifest->status() == library::FileStatus::Available);
  }

  TEST_CASE("ScanApplyOperation - independently updated identity does not stale a missing item",
            "[runtime][regression][scan-admission]")
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
    REQUIRE(operation.prepare());
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

    auto result = operation.run();

    REQUIRE(result);
    CHECK(result->missingCount == 1);
    CHECK(result->staleCount == 0);
    CHECK(result->failureCount == 0);
    CHECK(failures.count == 0);
    auto transaction = library.readTransaction();
    auto const optManifest = library.manifest().reader(transaction).get("song.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->status() == library::FileStatus::Missing);
    CHECK(optManifest->audioPayloadLength() == 42);
    CHECK(optManifest->audioSignature() == replacementSignature);
  }

  TEST_CASE("ScanApplyOperation - an occupied moved destination aborts all co-planned writes",
            "[runtime][regression][scan-admission]")
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
    REQUIRE(operation.prepare());

    auto occupier = library::test::makeEmptyTrackSpec("renamed.flac");
    occupier.title = "Concurrent destination";
    auto const occupyingTrackId = library::test::addTrack(library, occupier);
    auto result = operation.run();

    REQUIRE(result);
    CHECK(result->insertedIds.empty());
    CHECK(result->relinkedIds.empty());
    CHECK(result->staleCount == 0);
    CHECK(result->failureCount == 1);
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
            "[runtime][regression][scan-admission]")
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
    REQUIRE(operation.prepare());

    removeTrack(library, originalTrackId);
    auto result = operation.run();

    REQUIRE(result);
    CHECK(result->insertedIds.empty());
    CHECK(result->relinkedIds.empty());
    CHECK(result->staleCount == 0);
    CHECK(result->failureCount == 1);
    CHECK(failures.count == 1);
    auto transaction = library.readTransaction();
    CHECK_FALSE(library.tracks().reader(transaction).get(originalTrackId));
    auto manifestReader = library.manifest().reader(transaction);
    CHECK_FALSE(manifestReader.get("song.flac"));
    CHECK_FALSE(manifestReader.get("renamed.flac"));
    CHECK_FALSE(manifestReader.get("peer.flac"));
  }
} // namespace ao::rt::test
