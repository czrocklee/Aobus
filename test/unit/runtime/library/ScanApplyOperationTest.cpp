// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <runtime/library/ScanApplyOperation.h>

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackStoreTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/lmdb/Environment.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/utility/Hash128.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  using library::test::TrackSpec;
  using library::test::trackSpecFromView;
  using library::test::updateTrackSpec;

  namespace
  {
    // Counts the failures the operation pushes through its failure callback.
    struct FailureCounts final
    {
      std::int32_t failed = 0;

      std::move_only_function<void(ScanFailure const&)> callback()
      {
        return [this](ScanFailure const&) { ++failed; };
      }
    };

    [[noreturn]] void throwUnexpectedProgressFailure()
    {
      throw std::runtime_error{"unexpected progress failure"};
    }

    bool hasTag(TrackSpec const& spec, std::string_view name)
    {
      return std::ranges::any_of(spec.tags, [name](auto const& tag) { return tag == name; });
    }

    bool hasCustomMetadata(TrackSpec const& spec, std::string_view key, std::string_view value)
    {
      return std::ranges::any_of(spec.customMetadata,
                                 [&](auto const& entry)
                                 {
                                   auto const& [actualKey, actualValue] = entry;
                                   return actualKey == key && actualValue == value;
                                 });
    }

    std::uint64_t fileMtime(std::filesystem::path const& path)
    {
      return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::filesystem::last_write_time(path).time_since_epoch())
          .count());
    }

    std::vector<TrackId> changedTrackIds(ScanApplyResult const& result)
    {
      auto trackIds = result.insertedIds;
      trackIds.append_range(result.mutatedIds);
      trackIds.append_range(result.relinkedIds);
      return trackIds;
    }
  } // namespace

  TEST_CASE("ScanApplyOperation - initial scans process new files", "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const targetFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");

    auto scanner = LibraryScan{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.size() == 1);
    CHECK(plan.items()[0].classification == ScanClassification::New);

    auto counts = FailureCounts{};
    auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, counts.callback()};
    auto runRes = executor.run();
    REQUIRE(runRes);

    auto const& result = *runRes;
    CHECK(changedTrackIds(result).size() == 1);
    CHECK(result.insertedIds == changedTrackIds(result));
    CHECK(result.mutatedIds.empty());
    CHECK(result.relinkedIds.empty());
    CHECK(result.failureCount == 0);
    CHECK(counts.failed == 0);

    auto transaction = ml.readTransaction();
    auto const optView = ml.tracks().reader(transaction).get(changedTrackIds(result)[0]);
    REQUIRE(optView);
    CHECK(optView->metadata().title() == "Test Title");

    auto const manifestRes = ml.manifest().reader(transaction).get("song.flac");
    REQUIRE(manifestRes);
    CHECK(manifestRes->audioPayloadLength() > 0);
    CHECK(manifestRes->audioSignature() != utility::Hash128{});
  }

  TEST_CASE("ScanApplyOperation - deferred new scans write pending audio identity", "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const targetFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");

    auto scanner = LibraryScan{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.size() == 1);
    CHECK(plan.items()[0].classification == ScanClassification::New);

    auto counts = FailureCounts{};
    auto executor = ScanApplyOperation{ml,
                                       std::move(plan),
                                       nullptr,
                                       counts.callback(),
                                       ScanApplyOptions{.audioIdentityPolicy = AudioIdentityPolicy::DeferNew}};
    auto runRes = executor.run();
    REQUIRE(runRes);

    CHECK(changedTrackIds(*runRes).size() == 1);
    CHECK(runRes->failureCount == 0);
    CHECK(counts.failed == 0);

    auto transaction = ml.readTransaction();
    auto const manifestRes = ml.manifest().reader(transaction).get("song.flac");
    REQUIRE(manifestRes);
    CHECK(manifestRes->status() == library::FileStatus::Available);
    CHECK(manifestRes->fileSize() == std::filesystem::file_size(targetFile));
    CHECK(manifestRes->mtime() == fileMtime(targetFile));
    CHECK(manifestRes->audioPayloadLength() == 0);
    CHECK(manifestRes->audioSignature() == utility::Hash128{});
  }

  TEST_CASE("ScanApplyOperation - exhausted Track ids abort later scan work", "[runtime][regression][scan][atomicity]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    auto const databasePath = std::filesystem::path{temp.path()} / "db";
    std::filesystem::create_directories(musicRoot);

    // Establish the schema, then seed the largest valid Track id through the
    // raw storage test boundary so the next append deterministically exhausts
    // the integer key space.
    {
      std::ignore = library::test::makeTestMusicLibrary(musicRoot, databasePath);
    }
    {
      auto environment = lmdb::test::openEnvironment(
        databasePath,
        {.flags = lmdb::kEnvNoTls, .maxDatabases = 8, .mapSize = library::test::kTestMusicLibraryMapSize});
      auto transaction = lmdb::test::beginWriteTransaction(environment);
      auto hotDatabase = lmdb::test::openDatabase(transaction, "tracks_hot");
      auto coldDatabase = lmdb::test::openDatabase(transaction, "tracks_cold");
      constexpr auto kLastTrackId = std::numeric_limits<std::uint32_t>::max();
      REQUIRE(hotDatabase.writer(transaction).create(kLastTrackId, library::test::makeHotData({}, "Last Track")));
      REQUIRE(coldDatabase.writer(transaction).create(kLastTrackId, library::test::makeColdData({}, "missing.flac")));
      REQUIRE(transaction.commit());
    }

    auto ml = library::test::makeTestMusicLibrary(musicRoot, databasePath);
    constexpr auto kLastTrackId = std::numeric_limits<std::uint32_t>::max();
    {
      auto transaction = library::test::writeTransaction(ml);
      auto manifest = library::FileManifestBuilder::makeEmpty();
      manifest.trackId(TrackId{kLastTrackId}).status(library::FileStatus::Available).fileSize(1).mtime(1);
      REQUIRE(ml.manifest().writer(transaction).put("missing.flac", manifest.serialize()));
      REQUIRE(transaction.commit());
    }

    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), musicRoot / "new.flac");
    auto plan = LibraryScan{ml}.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::New) == 1);
    REQUIRE(plan.count(ScanClassification::Missing) == 1);
    auto const revisionBefore = ml.libraryRevision(ml.readTransaction());

    auto counts = FailureCounts{};
    auto operation = ScanApplyOperation{ml, std::move(plan), nullptr, counts.callback()};
    auto result = operation.run();

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::ResourceExhausted);
    CHECK(counts.failed == 0);

    auto transaction = ml.readTransaction();
    CHECK(ml.libraryRevision(transaction) == revisionBefore);
    CHECK_FALSE(ml.dictionary().findId("Test Artist"));
    REQUIRE(ml.tracks().reader(transaction).get(TrackId{kLastTrackId}));
    auto missingManifestRes = ml.manifest().reader(transaction).get("missing.flac");
    REQUIRE(missingManifestRes);
    CHECK(missingManifestRes->status() == library::FileStatus::Available);
    auto newManifestRes = ml.manifest().reader(transaction).get("new.flac");
    REQUIRE_FALSE(newManifestRes);
    CHECK(newManifestRes.error().code == Error::Code::NotFound);
  }

  TEST_CASE("ScanApplyOperation - missing persisted Track evidence aborts every planned item",
            "[runtime][regression][scan][atomicity]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const existingFile = musicRoot / "existing.flac";
    auto const newFile = musicRoot / "later-new.flac";
    std::filesystem::copy_file(sourceFile, existingFile);

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    auto initialPlan = LibraryScan{ml}.buildPlan().value();
    auto initialOperation = ScanApplyOperation{ml, std::move(initialPlan), nullptr, nullptr};
    auto initialRes = initialOperation.run();
    REQUIRE(initialRes);
    REQUIRE(initialRes->insertedIds.size() == 1);
    auto const existingTrackId = initialRes->insertedIds.front();

    {
      auto writableRes = library::WritableMusicLibrary::acquire(ml);
      REQUIRE(writableRes);
      auto transaction = writableRes->writeTransaction();
      REQUIRE(ml.tracks().writer(transaction).remove(existingTrackId));
      REQUIRE(transaction.commit());
    }

    std::filesystem::last_write_time(
      existingFile, std::filesystem::last_write_time(existingFile) + std::chrono::seconds{10});
    std::filesystem::copy_file(sourceFile, newFile);

    auto plan = LibraryScan{ml}.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::Changed) == 1);
    REQUIRE(plan.count(ScanClassification::New) == 1);
    auto const revisionBefore = ml.libraryRevision(ml.readTransaction());
    auto operation = ScanApplyOperation{ml, std::move(plan), nullptr, nullptr};
    auto result = operation.run();

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::CorruptData);

    auto transaction = ml.readTransaction();
    CHECK(ml.libraryRevision(transaction) == revisionBefore);
    CHECK_FALSE(ml.tracks().reader(transaction).get(existingTrackId));
    CHECK(ml.manifest().reader(transaction).get("existing.flac"));
    auto newManifestRes = ml.manifest().reader(transaction).get("later-new.flac");
    REQUIRE_FALSE(newManifestRes);
    CHECK(newManifestRes.error().code == Error::Code::NotFound);
  }

  TEST_CASE("ScanApplyOperation - apply requires prepared-file revalidation", "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), musicRoot / "song.flac");

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    auto plan = LibraryScan{ml}.buildPlan().value();
    auto operation = ScanApplyOperation{ml, std::move(plan), nullptr, nullptr};
    REQUIRE(operation.prepare());

    auto writableRes = library::WritableMusicLibrary::acquire(ml);
    REQUIRE(writableRes);
    auto transaction = writableRes->writeTransaction();
    auto applyRes = operation.apply(transaction);

    REQUIRE_FALSE(applyRes);
    CHECK(applyRes.error().code == Error::Code::InvalidState);
  }

  TEST_CASE("ScanApplyOperation - one revalidation permits exactly one apply", "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), musicRoot / "song.flac");

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    auto plan = LibraryScan{ml}.buildPlan().value();
    auto operation = ScanApplyOperation{ml, std::move(plan), nullptr, nullptr};
    REQUIRE(operation.prepare());
    REQUIRE(operation.revalidatePreparedFiles());
    REQUIRE(operation.readyForMutation());

    auto writableRes = library::WritableMusicLibrary::acquire(ml);
    REQUIRE(writableRes);
    auto transaction = writableRes->writeTransaction();
    auto firstApplyRes = operation.apply(transaction);
    REQUIRE(firstApplyRes);
    REQUIRE(firstApplyRes->insertedIds.size() == 1);

    auto secondApplyRes = operation.apply(transaction);
    REQUIRE_FALSE(secondApplyRes);
    CHECK(secondApplyRes.error().code == Error::Code::InvalidState);
    REQUIRE(transaction.commit());

    auto readTransaction = ml.readTransaction();
    auto reader = ml.tracks().reader(readTransaction);
    std::size_t trackCount = 0;

    for ([[maybe_unused]] auto const& entry : reader)
    {
      ++trackCount;
    }

    CHECK(trackCount == 1);
  }

  TEST_CASE("ScanApplyOperation - defer policy still uses cached new identity", "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const targetFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");

    {
      auto scanner = LibraryScan{ml};
      auto initialPlan = scanner.buildPlan().value();
      auto operation = ScanApplyOperation{ml, std::move(initialPlan), nullptr, nullptr};
      REQUIRE(operation.run());
    }

    auto const firstNewFile = musicRoot / "first-new.flac";
    auto const secondNewFile = musicRoot / "second-new.flac";
    std::filesystem::rename(targetFile, firstNewFile);
    std::filesystem::copy_file(sourceFile, secondNewFile);

    auto scanner = LibraryScan{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::New) == 2);
    REQUIRE(plan.count(ScanClassification::Missing) == 1);
    auto const planItems = plan.items();
    auto const firstNewItem =
      std::ranges::find_if(planItems, [](ScanItem const& item) { return item.uri == "first-new.flac"; });
    REQUIRE(firstNewItem != planItems.end());
    REQUIRE(hasAudioIdentity(*firstNewItem));
    auto const cachedPayloadLength = firstNewItem->audioPayloadLength;
    auto const cachedSignature = firstNewItem->audioSignature;

    auto counts = FailureCounts{};
    auto executor = ScanApplyOperation{ml,
                                       std::move(plan),
                                       nullptr,
                                       counts.callback(),
                                       ScanApplyOptions{.audioIdentityPolicy = AudioIdentityPolicy::DeferNew}};
    auto runRes = executor.run();
    REQUIRE(runRes);

    CHECK(runRes->insertedIds.size() == 2);
    CHECK(runRes->failureCount == 0);
    CHECK(counts.failed == 0);

    auto transaction = ml.readTransaction();
    auto const manifestRes = ml.manifest().reader(transaction).get("first-new.flac");
    REQUIRE(manifestRes);
    CHECK(manifestRes->audioPayloadLength() == cachedPayloadLength);
    CHECK(manifestRes->audioSignature() == cachedSignature);
  }

  TEST_CASE("ScanApplyOperation - reports fingerprint progress while hashing audio payload",
            "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    std::filesystem::copy_file(sourceFile, musicRoot / "song.flac");

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");

    auto scanner = LibraryScan{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.size() == 1);

    auto progressEvents = std::vector<ScanApplyProgress>{};
    auto progress = std::move_only_function<void(ScanApplyProgress const&)>{
      [&progressEvents](ScanApplyProgress const& progress) { progressEvents.push_back(progress); }};

    auto counts = FailureCounts{};
    auto executor = ScanApplyOperation{ml, std::move(plan), std::move(progress), counts.callback()};
    auto runRes = executor.run();
    REQUIRE(runRes);

    CHECK(changedTrackIds(*runRes).size() == 1);
    CHECK(runRes->failureCount == 0);
    REQUIRE(progressEvents.size() >= 3);
    CHECK(progressEvents[0].stage == ScanApplyProgressStage::Updating);
    CHECK(progressEvents[1].stage == ScanApplyProgressStage::Fingerprinting);
    CHECK(progressEvents[1].itemFraction == 0.0);
    CHECK(progressEvents.back().stage == ScanApplyProgressStage::Fingerprinting);
    CHECK(progressEvents.back().itemFraction == 1.0);
  }

  TEST_CASE("ScanApplyOperation - cancellation aborts partial scan transaction", "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    std::filesystem::copy_file(sourceFile, musicRoot / "first.flac");
    std::filesystem::copy_file(sourceFile, musicRoot / "second.flac");

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");

    auto scanner = LibraryScan{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.size() == 2);
    CHECK(plan.count(ScanClassification::New) == 2);

    auto stopSource = std::stop_source{};
    std::int32_t progressCount = 0;
    bool sawFingerprinting = false;
    auto progress = std::move_only_function<void(ScanApplyProgress const&)>{
      [&stopSource, &progressCount, &sawFingerprinting](ScanApplyProgress const& progress)
      {
        ++progressCount;

        if (progress.stage == ScanApplyProgressStage::Fingerprinting)
        {
          sawFingerprinting = true;
          stopSource.request_stop();
        }
      }};

    auto counts = FailureCounts{};
    auto executor = ScanApplyOperation{ml, std::move(plan), std::move(progress), counts.callback()};
    REQUIRE_THROWS_AS(executor.run(stopSource.get_token()), async::OperationCancelled);
    CHECK(executor.cancelled());
    CHECK(counts.failed == 0);
    CHECK(sawFingerprinting);
    CHECK(progressCount >= 2);

    auto transaction = ml.readTransaction();
    auto trackReader = ml.tracks().reader(transaction);
    auto manifestReader = ml.manifest().reader(transaction);
    CHECK(trackReader.begin() == trackReader.end());
    CHECK(manifestReader.begin() == manifestReader.end());
  }

  TEST_CASE("ScanApplyOperation - cancellation aborts between fingerprint chunks", "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const targetFile = musicRoot / "large.flac";
    std::filesystem::copy_file(sourceFile, targetFile);
    {
      auto out = std::ofstream{targetFile, std::ios::binary | std::ios::app};
      auto const padding = std::vector<char>(5ULL * 1024ULL * 1024ULL, '\0');
      out.write(padding.data(), static_cast<std::streamsize>(padding.size()));
    }

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");

    auto scanner = LibraryScan{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.size() == 1);

    auto stopSource = std::stop_source{};
    bool sawChunkProgress = false;
    auto progress = std::move_only_function<void(ScanApplyProgress const&)>{
      [&stopSource, &sawChunkProgress](ScanApplyProgress const& progress)
      {
        if (progress.stage == ScanApplyProgressStage::Fingerprinting && progress.itemFraction > 0.0 &&
            progress.itemFraction < 1.0)
        {
          sawChunkProgress = true;
          stopSource.request_stop();
        }
      }};

    auto counts = FailureCounts{};
    auto executor = ScanApplyOperation{ml, std::move(plan), std::move(progress), counts.callback()};
    REQUIRE_THROWS_AS(executor.run(stopSource.get_token()), async::OperationCancelled);
    CHECK(executor.cancelled());
    CHECK(counts.failed == 0);
    CHECK(sawChunkProgress);

    auto transaction = ml.readTransaction();
    auto trackReader = ml.tracks().reader(transaction);
    auto manifestReader = ml.manifest().reader(transaction);
    CHECK(trackReader.begin() == trackReader.end());
    CHECK(manifestReader.begin() == manifestReader.end());
  }

  TEST_CASE("ScanApplyOperation - cancellation clears failures from the aborted transaction",
            "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    std::filesystem::copy_file(sourceFile, musicRoot / "first.flac");
    std::filesystem::copy_file(sourceFile, musicRoot / "second.flac");

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");

    auto scanner = LibraryScan{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.size() == 2);
    auto const corruptPath = plan.items()[0].fullPath;
    auto const cancelPath = plan.items()[1].fullPath;
    {
      auto out = std::ofstream{corruptPath, std::ios::binary | std::ios::trunc};
      out << "NOT A FLAC FILE";
    }

    auto stopSource = std::stop_source{};
    auto progress = std::move_only_function<void(ScanApplyProgress const&)>{
      [&stopSource, &cancelPath](ScanApplyProgress const& progress)
      {
        if (progress.path == cancelPath && progress.stage == ScanApplyProgressStage::Fingerprinting)
        {
          stopSource.request_stop();
        }
      }};

    auto counts = FailureCounts{};
    auto executor = ScanApplyOperation{ml, std::move(plan), std::move(progress), counts.callback()};
    REQUIRE_THROWS_AS(executor.run(stopSource.get_token()), async::OperationCancelled);
    CHECK(executor.cancelled());
    CHECK(counts.failed == 1);
  }

  TEST_CASE("ScanApplyOperation - skips unchanged files", "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const targetFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");

    // First scan to populate the manifest
    {
      auto scanner = LibraryScan{ml};
      auto plan = scanner.buildPlan().value();
      auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, nullptr};
      auto runRes = executor.run();
      REQUIRE(runRes);
    }

    // Second scan should find unchanged file
    auto scanner = LibraryScan{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.size() == 1);
    CHECK(plan.items()[0].classification == ScanClassification::Unchanged);

    auto counts = FailureCounts{};
    auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, counts.callback()};
    auto runRes = executor.run();
    REQUIRE(runRes);

    auto const& result = *runRes;
    // An unchanged file is skipped silently: nothing processed, nothing reported.
    CHECK(changedTrackIds(result).empty());
    CHECK(result.failureCount == 0);
    CHECK(counts.failed == 0);
  }

  TEST_CASE("ScanApplyOperation - updates changed files", "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const targetFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");

    // First scan to populate the manifest
    {
      auto scanner = LibraryScan{ml};
      auto plan = scanner.buildPlan().value();
      auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, nullptr};
      auto runRes = executor.run();
      REQUIRE(runRes);
    }

    std::uint64_t oldPayloadLength = 0;
    auto oldSignature = utility::Hash128{};
    {
      auto transaction = ml.readTransaction();
      auto const manifestRes = ml.manifest().reader(transaction).get("song.flac");
      REQUIRE(manifestRes);
      oldPayloadLength = manifestRes->audioPayloadLength();
      oldSignature = manifestRes->audioSignature();
    }

    // Modify targetFile and advance mtime
    auto const oldMTime = std::filesystem::last_write_time(targetFile);
    {
      auto out = std::ofstream{targetFile, std::ios::binary | std::ios::app};
      out << "some extra garbage";
    }
    std::filesystem::last_write_time(targetFile, oldMTime + std::chrono::seconds{10});

    auto scanner = LibraryScan{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.size() == 1);
    CHECK(plan.items()[0].classification == ScanClassification::Changed);

    auto counts = FailureCounts{};
    auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, counts.callback()};
    auto runRes = executor.run();
    REQUIRE(runRes);

    auto const& result = *runRes;
    CHECK(changedTrackIds(result).size() == 1);
    CHECK(result.insertedIds.empty());
    CHECK(result.mutatedIds == changedTrackIds(result));
    CHECK(result.relinkedIds.empty());
    CHECK(result.failureCount == 0);
    CHECK(counts.failed == 0);

    auto transaction = ml.readTransaction();
    auto const manifestRes = ml.manifest().reader(transaction).get("song.flac");
    REQUIRE(manifestRes);
    auto const actualMtime =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::filesystem::last_write_time(targetFile).time_since_epoch())
                                   .count());
    CHECK(manifestRes->mtime() == actualMtime);
    CHECK(manifestRes->audioPayloadLength() > oldPayloadLength);
    CHECK(manifestRes->audioSignature() != oldSignature);
  }

  TEST_CASE("ScanApplyOperation - updates manifest status for missing files", "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const targetFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");

    // First scan to populate the manifest
    {
      auto scanner = LibraryScan{ml};
      auto plan = scanner.buildPlan().value();
      auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, nullptr};
      auto runRes = executor.run();
      REQUIRE(runRes);
    }

    // Remove the file
    std::filesystem::remove(targetFile);

    auto scanner = LibraryScan{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.size() == 1);
    CHECK(plan.items()[0].classification == ScanClassification::Missing);

    auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, nullptr};
    auto runRes = executor.run();
    REQUIRE(runRes);

    auto transaction = ml.readTransaction();
    auto const manifestRes = ml.manifest().reader(transaction).get("song.flac");
    REQUIRE(manifestRes);
    CHECK(manifestRes->status() == library::FileStatus::Missing);
    CHECK(changedTrackIds(*runRes).empty());
    CHECK(runRes->missingCount == 1);
    CHECK(runRes->failureCount == 0);
  }

  TEST_CASE("ScanApplyOperation - relinks moved files while preserving DB-owned curation",
            "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const originalFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, originalFile);

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");

    auto originalTrackId = kInvalidTrackId;

    {
      auto scanner = LibraryScan{ml};
      auto plan = scanner.buildPlan().value();
      auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, nullptr};
      auto runRes = executor.run();
      REQUIRE(runRes);
      REQUIRE(changedTrackIds(*runRes).size() == 1);
      originalTrackId = changedTrackIds(*runRes).front();
    }

    std::uint64_t originalPayloadLength = 0;
    auto originalSignature = utility::Hash128{};
    {
      auto transaction = ml.readTransaction();
      auto const manifestRes = ml.manifest().reader(transaction).get("song.flac");
      REQUIRE(manifestRes);
      originalPayloadLength = manifestRes->audioPayloadLength();
      originalSignature = manifestRes->audioSignature();
    }

    updateTrackSpec(ml,
                    originalTrackId,
                    [](TrackSpec& spec)
                    {
                      spec.title = "Curated Title";
                      spec.tags = {"favorite"};
                      spec.customMetadata = {{"catalog", "AOB-42"}};
                    });

    auto orderedListId = kInvalidListId;
    {
      auto transaction = library::test::writeTransaction(ml);
      auto listBuilder = library::ListBuilder::makeEmpty();
      listBuilder.name("Ordered").orderTrackIds().add(originalTrackId);
      auto createRes = ml.lists().writer(transaction).create(ao::test::requireValue(listBuilder.serialize()));
      REQUIRE(createRes);
      orderedListId = *createRes;
      REQUIRE(transaction.commit());
    }

    auto const movedFile = musicRoot / "renamed" / "song.flac";
    std::filesystem::create_directories(movedFile.parent_path());
    std::filesystem::rename(originalFile, movedFile);

    auto scanner = LibraryScan{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.size() == 1);
    CHECK(plan.items().front().classification == ScanClassification::Moved);
    CHECK(plan.items().front().oldUri == "song.flac");
    CHECK(plan.items().front().trackId == originalTrackId);

    bool sawFingerprinting = false;
    auto progressFractions = std::vector<double>{};
    auto progress = std::move_only_function<void(ScanApplyProgress const&)>{
      [&sawFingerprinting, &progressFractions](ScanApplyProgress const& progress)
      {
        progressFractions.push_back(progress.itemFraction);

        if (progress.stage == ScanApplyProgressStage::Fingerprinting)
        {
          sawFingerprinting = true;
        }
      }};

    auto counts = FailureCounts{};
    auto executor = ScanApplyOperation{ml, std::move(plan), std::move(progress), counts.callback()};
    auto runRes = executor.run();
    REQUIRE(runRes);

    REQUIRE(changedTrackIds(*runRes).size() == 1);
    CHECK(changedTrackIds(*runRes).front() == originalTrackId);
    CHECK(runRes->insertedIds.empty());
    CHECK(runRes->mutatedIds.empty());
    CHECK(runRes->relinkedIds == std::vector{originalTrackId});
    CHECK(runRes->relinkedIds.size() == 1);
    CHECK(runRes->failureCount == 0);
    CHECK(counts.failed == 0);
    CHECK(sawFingerprinting);
    CHECK(std::ranges::is_sorted(progressFractions));

    auto transaction = ml.readTransaction();
    auto trackReader = ml.tracks().reader(transaction);
    auto const optView = trackReader.get(originalTrackId, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);

    auto const spec = trackSpecFromView(ml, *optView);
    CHECK(spec.uri == "renamed/song.flac");
    CHECK(spec.title == "Curated Title");
    CHECK(hasTag(spec, "favorite"));
    CHECK(hasCustomMetadata(spec, "catalog", "AOB-42"));

    auto manifestReader = ml.manifest().reader(transaction);
    auto const oldManifestRes = manifestReader.get("song.flac");
    REQUIRE_FALSE(oldManifestRes);
    CHECK(oldManifestRes.error().code == Error::Code::NotFound);

    auto const newManifestRes = manifestReader.get("renamed/song.flac");
    REQUIRE(newManifestRes);
    CHECK(newManifestRes->trackId() == originalTrackId);
    CHECK(newManifestRes->status() == library::FileStatus::Available);
    CHECK(newManifestRes->audioPayloadLength() == originalPayloadLength);
    CHECK(newManifestRes->audioSignature() == originalSignature);

    auto const optOrderedList = ml.lists().reader(transaction).get(orderedListId);
    REQUIRE(optOrderedList);
    REQUIRE(optOrderedList->orderTrackIds().size() == 1);
    CHECK(optOrderedList->orderTrackIds()[0] == originalTrackId);
  }

  TEST_CASE("ScanApplyOperation - rejects moved files whose live identity changed after preparation",
            "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const originalFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, originalFile);

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");

    auto originalTrackId = kInvalidTrackId;

    {
      auto scanner = LibraryScan{ml};
      auto plan = scanner.buildPlan().value();
      auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, nullptr};
      auto runRes = executor.run();
      REQUIRE(runRes);
      REQUIRE(changedTrackIds(*runRes).size() == 1);
      originalTrackId = changedTrackIds(*runRes).front();
    }

    auto const movedFile = musicRoot / "renamed.flac";
    std::filesystem::rename(originalFile, movedFile);

    auto scanner = LibraryScan{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.size() == 1);
    REQUIRE(plan.items().front().classification == ScanClassification::Moved);

    bool sawFingerprinting = false;
    auto progress = std::move_only_function<void(ScanApplyProgress const&)>{
      [&sawFingerprinting](ScanApplyProgress const& progress)
      {
        if (progress.stage == ScanApplyProgressStage::Fingerprinting)
        {
          sawFingerprinting = true;
        }
      }};

    auto counts = FailureCounts{};
    auto executor = ScanApplyOperation{ml,
                                       std::move(plan),
                                       std::move(progress),
                                       counts.callback(),
                                       ScanApplyOptions{.audioIdentityPolicy = AudioIdentityPolicy::DeferNew}};
    auto prepareRes = executor.prepare();
    REQUIRE(prepareRes);
    REQUIRE(sawFingerprinting);
    REQUIRE(prepareRes->failureCount == 0);

    auto const differentAudio = audio::test::requireAudioFixture("hires.flac");
    std::filesystem::copy_file(differentAudio, movedFile, std::filesystem::copy_options::overwrite_existing);

    auto runRes = executor.run();
    REQUIRE(runRes);

    CHECK(changedTrackIds(*runRes).empty());
    CHECK(runRes->relinkedIds.empty());
    CHECK(runRes->failureCount == 1);
    CHECK(counts.failed == 1);
    CHECK(sawFingerprinting);

    auto transaction = ml.readTransaction();
    auto const optView =
      ml.tracks().reader(transaction).get(originalTrackId, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);
    CHECK(optView->property().uri() == "song.flac");

    auto manifestReader = ml.manifest().reader(transaction);
    CHECK(manifestReader.get("song.flac"));
    auto const newManifestRes = manifestReader.get("renamed.flac");
    REQUIRE_FALSE(newManifestRes);
    CHECK(newManifestRes.error().code == Error::Code::NotFound);
  }

  TEST_CASE("ScanApplyOperation - moved-file revalidation failure aborts co-planned inserts",
            "[runtime][regression][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const originalFile = musicRoot / "song.flac";
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), originalFile);

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    auto initialPlan = LibraryScan{ml}.buildPlan().value();
    auto initialRes = ScanApplyOperation{ml, std::move(initialPlan), nullptr, nullptr}.run();
    REQUIRE(initialRes);
    REQUIRE(initialRes->insertedIds.size() == 1);
    auto const originalTrackId = initialRes->insertedIds.front();

    auto const movedFile = musicRoot / "renamed.flac";
    auto const newFile = musicRoot / "new.flac";
    std::filesystem::rename(originalFile, movedFile);
    std::filesystem::copy_file(audio::test::requireAudioFixture("hires.flac"), newFile);

    auto plan = LibraryScan{ml}.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::Moved) == 1);
    REQUIRE(plan.count(ScanClassification::New) == 1);

    auto counts = FailureCounts{};
    auto operation = ScanApplyOperation{ml, std::move(plan), nullptr, counts.callback()};
    auto prepareRes = operation.prepare();
    REQUIRE(prepareRes);
    REQUIRE(prepareRes->failureCount == 0);

    auto const revisionBeforeRevalidation = [&]
    {
      auto transaction = ml.readTransaction();
      return ml.libraryRevision(transaction);
    }();

    std::filesystem::copy_file(
      audio::test::requireAudioFixture("hires.flac"), movedFile, std::filesystem::copy_options::overwrite_existing);

    auto runRes = operation.run();
    REQUIRE(runRes);
    CHECK(runRes->insertedIds.empty());
    CHECK(runRes->mutatedIds.empty());
    CHECK(runRes->relinkedIds.empty());
    CHECK(runRes->failureCount == 1);
    CHECK(counts.failed == 1);

    auto transaction = ml.readTransaction();
    CHECK(ml.libraryRevision(transaction) == revisionBeforeRevalidation);
    auto trackReader = ml.tracks().reader(transaction);
    std::size_t trackCount = 0;

    for ([[maybe_unused]] auto const& entry : trackReader)
    {
      ++trackCount;
    }

    CHECK(trackCount == 1);
    auto const optOriginalTrack = trackReader.get(originalTrackId, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optOriginalTrack);
    CHECK(optOriginalTrack->property().uri() == "song.flac");

    auto manifestReader = ml.manifest().reader(transaction);
    CHECK(manifestReader.get("song.flac"));
    CHECK_FALSE(manifestReader.get("renamed.flac"));
    CHECK_FALSE(manifestReader.get("new.flac"));
  }

  TEST_CASE("ScanApplyOperation - rejects a plan after the library revision changes", "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    std::filesystem::copy_file(sourceFile, musicRoot / "song.flac");

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    auto scanner = LibraryScan{ml};
    auto plan = scanner.buildPlan().value();
    auto counts = FailureCounts{};
    auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, counts.callback()};
    REQUIRE(executor.prepare());

    {
      auto writableRes = library::WritableMusicLibrary::acquire(ml);
      REQUIRE(writableRes);
      auto transaction = writableRes->writeTransaction();
      REQUIRE(transaction.commit());
    }

    auto runRes = executor.run();
    REQUIRE_FALSE(runRes);
    CHECK(runRes.error().code == Error::Code::Conflict);
    CHECK(counts.failed == 0);

    auto transaction = ml.readTransaction();
    auto trackReader = ml.tracks().reader(transaction);
    CHECK(trackReader.begin() == trackReader.end());
    CHECK_FALSE(ml.manifest().reader(transaction).get("song.flac"));
  }

  TEST_CASE("ScanApplyOperation - rejects a plan built for another library", "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const firstRoot = std::filesystem::path{temp.path()} / "first-music";
    auto const secondRoot = std::filesystem::path{temp.path()} / "second-music";
    std::filesystem::create_directories(firstRoot);
    std::filesystem::create_directories(secondRoot);
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    std::filesystem::copy_file(sourceFile, firstRoot / "song.flac");

    auto firstLibrary = library::test::makeTestMusicLibrary(firstRoot, std::filesystem::path{temp.path()} / "first-db");
    auto secondLibrary =
      library::test::makeTestMusicLibrary(secondRoot, std::filesystem::path{temp.path()} / "second-db");
    auto plan = LibraryScan{firstLibrary}.buildPlan().value();
    bool sawProgress = false;

    auto operation = ScanApplyOperation{
      secondLibrary, std::move(plan), [&sawProgress](ScanApplyProgress const&) { sawProgress = true; }, nullptr};
    auto result = operation.run();

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidInput);
    CHECK_FALSE(sawProgress);
    auto transaction = secondLibrary.readTransaction();
    auto trackReader = secondLibrary.tracks().reader(transaction);
    CHECK(trackReader.begin() == trackReader.end());
  }

  TEST_CASE("ScanApplyOperation - rejects a second plan from an already applied snapshot",
            "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    std::filesystem::copy_file(sourceFile, musicRoot / "song.flac");

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    auto firstPlan = LibraryScan{ml}.buildPlan().value();
    auto secondPlan = LibraryScan{ml}.buildPlan().value();

    auto firstRes = ScanApplyOperation{ml, std::move(firstPlan), nullptr, nullptr}.run();
    REQUIRE(firstRes);
    REQUIRE(firstRes->insertedIds.size() == 1);

    bool sawReplayProgress = false;
    auto secondOperation = ScanApplyOperation{
      ml, std::move(secondPlan), [&sawReplayProgress](ScanApplyProgress const&) { sawReplayProgress = true; }, nullptr};
    auto secondRes = secondOperation.run();
    REQUIRE_FALSE(secondRes);
    CHECK(secondRes.error().code == Error::Code::Conflict);
    CHECK_FALSE(sawReplayProgress);

    auto transaction = ml.readTransaction();
    auto trackReader = ml.tracks().reader(transaction);
    std::size_t trackCount = 0;

    for ([[maybe_unused]] auto const& entry : trackReader)
    {
      ++trackCount;
    }

    CHECK(trackCount == 1);
    auto const manifestRes = ml.manifest().reader(transaction).get("song.flac");
    REQUIRE(manifestRes);
    CHECK(manifestRes->trackId() == firstRes->insertedIds.front());
  }

  TEST_CASE("ScanApplyOperation - reports corrupted file failures", "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const corruptedFile = musicRoot / "corrupted.flac";
    {
      auto out = std::ofstream{corruptedFile, std::ios::binary};
      out << "NOT A FLAC FILE";
    }

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");

    auto scanner = LibraryScan{ml};
    auto plan = scanner.buildPlan().value();

    auto counts = FailureCounts{};
    auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, counts.callback()};
    auto runRes = executor.run();
    REQUIRE(runRes);

    auto const& result = *runRes;
    CHECK(counts.failed == 1);
    CHECK(result.failureCount == 1);
    CHECK(changedTrackIds(result).empty());
  }

  TEST_CASE("ScanApplyOperation - propagates unexpected process exceptions", "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    std::filesystem::copy_file(sourceFile, musicRoot / "bad.flac");
    auto scanner = LibraryScan{ml};
    auto plan = scanner.buildPlan().value();

    auto counts = FailureCounts{};
    auto thrower = [](ScanApplyProgress const&) { throwUnexpectedProgressFailure(); };
    auto executor = ScanApplyOperation{ml, std::move(plan), std::move(thrower), counts.callback()};

    REQUIRE_THROWS_AS(std::ignore = executor.run(), std::runtime_error);
    CHECK(counts.failed == 0);
  }

  TEST_CASE("ScanApplyOperation - ignores non-decodable files omitted from the plan", "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    // A text file, plus audio formats we have no reader for. The scanner only
    // admits decodable extensions, so none of these reach the executor.
    for (auto const* const name : {"notes.txt", "cover.jpg", "song.ogg", "song.alac"})
    {
      auto out = std::ofstream{musicRoot / name, std::ios::binary};
      out << "not a supported audio file";
    }

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");

    auto scanner = LibraryScan{ml};
    auto plan = scanner.buildPlan().value();
    CHECK(plan.empty());

    auto counts = FailureCounts{};
    auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, counts.callback()};
    auto runRes = executor.run();
    REQUIRE(runRes);

    CHECK(changedTrackIds(*runRes).empty());
    CHECK(runRes->failureCount == 0);
    CHECK(counts.failed == 0);
  }
} // namespace ao::rt::test
