// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryScanner.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ScanPlanExecutor.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/utility/Fnv1a.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::library::test
{
  namespace
  {
    // Counts the failures the executor pushes through its ItemFailureCallback.
    struct FailureCounts final
    {
      std::int32_t failed = 0;

      ScanPlanExecutor::ItemFailureCallback callback()
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

    ScanItem newAudioItem(std::filesystem::path const& fullPath, std::string_view uri)
    {
      return ScanItem{.uri = std::string{uri},
                      .oldUri = {},
                      .fullPath = fullPath,
                      .classification = ScanClassification::New,
                      .fileSize = std::filesystem::file_size(fullPath),
                      .mtime = fileMtime(fullPath),
                      .audioPayloadLength = 0,
                      .audioSignature = {},
                      .trackId = kInvalidTrackId,
                      .errorMessage = {}};
    }
  } // namespace

  TEST_CASE("ScanPlanExecutor - initial scans process new files", "[library][unit][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const targetFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);

    auto ml = MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};

    auto scanner = LibraryScanner{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items[0].classification == ScanClassification::New);

    auto counts = FailureCounts{};
    auto executor = ScanPlanExecutor{ml, std::move(plan), nullptr, counts.callback()};
    auto runResult = executor.run();
    REQUIRE(runResult);

    auto const& result = *runResult;
    CHECK(result.processedIds.size() == 1);
    CHECK(result.failureCount == 0);
    CHECK(counts.failed == 0);

    auto txn = ml.readTransaction();
    auto const optView = ml.tracks().reader(txn).get(result.processedIds[0]);
    REQUIRE(optView);
    CHECK(optView->metadata().title() == "Test Title");

    auto const manifestResult = ml.manifest().reader(txn).get("song.flac");
    REQUIRE(manifestResult);
    CHECK(manifestResult->audioPayloadLength() > 0);
    CHECK(manifestResult->audioSignature() != utility::Hash128{});
  }

  TEST_CASE("ScanPlanExecutor - reports fingerprint progress while hashing audio payload", "[library][unit][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    std::filesystem::copy_file(sourceFile, musicRoot / "song.flac");

    auto ml = MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};

    auto scanner = LibraryScanner{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.items.size() == 1);

    auto progressEvents = std::vector<ScanPlanExecutor::Progress>{};
    auto progress = ScanPlanExecutor::ProgressCallback{[&progressEvents](ScanPlanExecutor::Progress const& progress)
                                                       { progressEvents.push_back(progress); }};

    auto counts = FailureCounts{};
    auto executor = ScanPlanExecutor{ml, std::move(plan), std::move(progress), counts.callback()};
    auto runResult = executor.run();
    REQUIRE(runResult);

    CHECK(runResult->processedIds.size() == 1);
    CHECK(runResult->failureCount == 0);
    REQUIRE(progressEvents.size() >= 3);
    CHECK(progressEvents[0].stage == ScanPlanExecutor::ProgressStage::Updating);
    CHECK(progressEvents[1].stage == ScanPlanExecutor::ProgressStage::Fingerprinting);
    CHECK(progressEvents[1].itemFraction == 0.0);
    CHECK(progressEvents.back().stage == ScanPlanExecutor::ProgressStage::Fingerprinting);
    CHECK(progressEvents.back().itemFraction == 1.0);
  }

  TEST_CASE("ScanPlanExecutor - cancellation aborts partial scan transaction", "[library][unit][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    std::filesystem::copy_file(sourceFile, musicRoot / "first.flac");
    std::filesystem::copy_file(sourceFile, musicRoot / "second.flac");

    auto ml = MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};

    auto scanner = LibraryScanner{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.items.size() == 2);
    CHECK(plan.count(ScanClassification::New) == 2);

    auto stopSource = std::stop_source{};
    std::int32_t progressCount = 0;
    bool sawFingerprinting = false;
    auto progress = ScanPlanExecutor::ProgressCallback{
      [&stopSource, &progressCount, &sawFingerprinting](ScanPlanExecutor::Progress const& progress)
      {
        ++progressCount;

        if (progress.stage == ScanPlanExecutor::ProgressStage::Fingerprinting)
        {
          sawFingerprinting = true;
          stopSource.request_stop();
        }
      }};

    auto counts = FailureCounts{};
    auto executor = ScanPlanExecutor{ml, std::move(plan), std::move(progress), counts.callback()};
    auto runResult = executor.run(stopSource.get_token());
    REQUIRE(runResult);

    CHECK(runResult->cancelled);
    CHECK(runResult->processedIds.empty());
    CHECK(runResult->relinkedCount == 0);
    CHECK(runResult->missingCount == 0);
    CHECK(runResult->failureCount == 0);
    CHECK(counts.failed == 0);
    CHECK(sawFingerprinting);
    CHECK(progressCount >= 2);

    auto txn = ml.readTransaction();
    auto trackReader = ml.tracks().reader(txn);
    auto manifestReader = ml.manifest().reader(txn);
    CHECK(trackReader.begin() == trackReader.end());
    CHECK(manifestReader.begin() == manifestReader.end());
  }

  TEST_CASE("ScanPlanExecutor - cancellation aborts between fingerprint chunks", "[library][unit][scan]")
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

    auto ml = MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};

    auto scanner = LibraryScanner{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.items.size() == 1);

    auto stopSource = std::stop_source{};
    bool sawChunkProgress = false;
    auto progress =
      ScanPlanExecutor::ProgressCallback{[&stopSource, &sawChunkProgress](ScanPlanExecutor::Progress const& progress)
                                         {
                                           if (progress.stage == ScanPlanExecutor::ProgressStage::Fingerprinting &&
                                               progress.itemFraction > 0.0 && progress.itemFraction < 1.0)
                                           {
                                             sawChunkProgress = true;
                                             stopSource.request_stop();
                                           }
                                         }};

    auto counts = FailureCounts{};
    auto executor = ScanPlanExecutor{ml, std::move(plan), std::move(progress), counts.callback()};
    auto runResult = executor.run(stopSource.get_token());
    REQUIRE(runResult);

    CHECK(runResult->cancelled);
    CHECK(runResult->processedIds.empty());
    CHECK(runResult->failureCount == 0);
    CHECK(counts.failed == 0);
    CHECK(sawChunkProgress);

    auto txn = ml.readTransaction();
    auto trackReader = ml.tracks().reader(txn);
    auto manifestReader = ml.manifest().reader(txn);
    CHECK(trackReader.begin() == trackReader.end());
    CHECK(manifestReader.begin() == manifestReader.end());
  }

  TEST_CASE("ScanPlanExecutor - cancellation clears failures from the aborted transaction", "[library][unit][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const targetFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);

    auto ml = MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};

    auto plan = ScanPlan{};
    plan.items.push_back(ScanItem{.uri = "corrupted.flac",
                                  .oldUri = {},
                                  .fullPath = {},
                                  .classification = ScanClassification::Error,
                                  .fileSize = 0,
                                  .mtime = 0,
                                  .audioPayloadLength = 0,
                                  .audioSignature = {},
                                  .trackId = kInvalidTrackId,
                                  .errorMessage = "corrupt input"});
    plan.items.push_back(newAudioItem(targetFile, "song.flac"));

    auto stopSource = std::stop_source{};
    auto progress =
      ScanPlanExecutor::ProgressCallback{[&stopSource](ScanPlanExecutor::Progress const& progress)
                                         {
                                           if (progress.stage == ScanPlanExecutor::ProgressStage::Fingerprinting)
                                           {
                                             stopSource.request_stop();
                                           }
                                         }};

    auto counts = FailureCounts{};
    auto executor = ScanPlanExecutor{ml, std::move(plan), std::move(progress), counts.callback()};
    auto runResult = executor.run(stopSource.get_token());
    REQUIRE(runResult);

    CHECK(runResult->cancelled);
    CHECK(runResult->processedIds.empty());
    CHECK(runResult->failureCount == 0);
    CHECK(counts.failed == 1);
  }

  TEST_CASE("ScanPlanExecutor - skips unchanged files", "[library][unit][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const targetFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);

    auto ml = MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};

    // First scan to populate the manifest
    {
      auto scanner = LibraryScanner{ml};
      auto plan = scanner.buildPlan().value();
      auto executor = ScanPlanExecutor{ml, std::move(plan), nullptr, nullptr};
      auto runResult = executor.run();
      REQUIRE(runResult);
    }

    // Second scan should find unchanged file
    auto scanner = LibraryScanner{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items[0].classification == ScanClassification::Unchanged);

    auto counts = FailureCounts{};
    auto executor = ScanPlanExecutor{ml, std::move(plan), nullptr, counts.callback()};
    auto runResult = executor.run();
    REQUIRE(runResult);

    auto const& result = *runResult;
    // An unchanged file is skipped silently: nothing processed, nothing reported.
    CHECK(result.processedIds.empty());
    CHECK(result.failureCount == 0);
    CHECK(counts.failed == 0);
  }

  TEST_CASE("ScanPlanExecutor - updates changed files", "[library][unit][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const targetFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);

    auto ml = MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};

    // First scan to populate the manifest
    {
      auto scanner = LibraryScanner{ml};
      auto plan = scanner.buildPlan().value();
      auto executor = ScanPlanExecutor{ml, std::move(plan), nullptr, nullptr};
      auto runResult = executor.run();
      REQUIRE(runResult);
    }

    std::uint64_t oldPayloadLength = 0;
    auto oldSignature = utility::Hash128{};
    {
      auto txn = ml.readTransaction();
      auto const manifestResult = ml.manifest().reader(txn).get("song.flac");
      REQUIRE(manifestResult);
      oldPayloadLength = manifestResult->audioPayloadLength();
      oldSignature = manifestResult->audioSignature();
    }

    // Modify targetFile and advance mtime
    auto const oldMTime = std::filesystem::last_write_time(targetFile);
    {
      auto out = std::ofstream{targetFile, std::ios::binary | std::ios::app};
      out << "some extra garbage";
    }
    std::filesystem::last_write_time(targetFile, oldMTime + std::chrono::seconds{10});

    auto scanner = LibraryScanner{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items[0].classification == ScanClassification::Changed);

    auto counts = FailureCounts{};
    auto executor = ScanPlanExecutor{ml, std::move(plan), nullptr, counts.callback()};
    auto runResult = executor.run();
    REQUIRE(runResult);

    auto const& result = *runResult;
    CHECK(result.processedIds.size() == 1);
    CHECK(result.failureCount == 0);
    CHECK(counts.failed == 0);

    auto txn = ml.readTransaction();
    auto const manifestResult = ml.manifest().reader(txn).get("song.flac");
    REQUIRE(manifestResult);
    auto const actualMtime =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::filesystem::last_write_time(targetFile).time_since_epoch())
                                   .count());
    CHECK(manifestResult->mtime() == actualMtime);
    CHECK(manifestResult->audioPayloadLength() > oldPayloadLength);
    CHECK(manifestResult->audioSignature() != oldSignature);
  }

  TEST_CASE("ScanPlanExecutor - updates manifest status for missing files", "[library][unit][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const targetFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);

    auto ml = MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};

    // First scan to populate the manifest
    {
      auto scanner = LibraryScanner{ml};
      auto plan = scanner.buildPlan().value();
      auto executor = ScanPlanExecutor{ml, std::move(plan), nullptr, nullptr};
      auto runResult = executor.run();
      REQUIRE(runResult);
    }

    // Remove the file
    std::filesystem::remove(targetFile);

    auto scanner = LibraryScanner{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items[0].classification == ScanClassification::Missing);

    auto executor = ScanPlanExecutor{ml, std::move(plan), nullptr, nullptr};
    auto runResult = executor.run();
    REQUIRE(runResult);

    auto txn = ml.readTransaction();
    auto const manifestResult = ml.manifest().reader(txn).get("song.flac");
    REQUIRE(manifestResult);
    CHECK(manifestResult->status() == FileStatus::Missing);
    CHECK(runResult->processedIds.empty());
    CHECK(runResult->missingCount == 1);
    CHECK(runResult->failureCount == 0);
  }

  TEST_CASE("ScanPlanExecutor - relinks moved files while preserving DB-owned curation", "[library][unit][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const originalFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, originalFile);

    auto ml = MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};

    auto originalTrackId = kInvalidTrackId;

    {
      auto scanner = LibraryScanner{ml};
      auto plan = scanner.buildPlan().value();
      auto executor = ScanPlanExecutor{ml, std::move(plan), nullptr, nullptr};
      auto runResult = executor.run();
      REQUIRE(runResult);
      REQUIRE(runResult->processedIds.size() == 1);
      originalTrackId = runResult->processedIds.front();
    }

    std::uint64_t originalPayloadLength = 0;
    auto originalSignature = utility::Hash128{};
    {
      auto txn = ml.readTransaction();
      auto const manifestResult = ml.manifest().reader(txn).get("song.flac");
      REQUIRE(manifestResult);
      originalPayloadLength = manifestResult->audioPayloadLength();
      originalSignature = manifestResult->audioSignature();
    }

    updateTrackSpec(ml,
                    originalTrackId,
                    [](TrackSpec& spec)
                    {
                      spec.title = "Curated Title";
                      spec.tags = {"favorite"};
                      spec.customMetadata = {{"catalog", "AOB-42"}};
                    });

    auto manualListId = kInvalidListId;
    {
      auto txn = ml.writeTransaction();
      auto listBuilder = ListBuilder::createNew();
      listBuilder.name("Manual").tracks().add(originalTrackId);
      auto createResult = ml.lists().writer(txn).create(listBuilder.serialize());
      REQUIRE(createResult);
      manualListId = createResult->first;
      REQUIRE(txn.commit());
    }

    auto const movedFile = musicRoot / "renamed" / "song.flac";
    std::filesystem::create_directories(movedFile.parent_path());
    std::filesystem::rename(originalFile, movedFile);

    auto scanner = LibraryScanner{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items.front().classification == ScanClassification::Moved);
    CHECK(plan.items.front().oldUri == "song.flac");
    CHECK(plan.items.front().trackId == originalTrackId);

    bool sawFingerprinting = false;
    auto progress =
      ScanPlanExecutor::ProgressCallback{[&sawFingerprinting](ScanPlanExecutor::Progress const& progress)
                                         {
                                           if (progress.stage == ScanPlanExecutor::ProgressStage::Fingerprinting)
                                           {
                                             sawFingerprinting = true;
                                           }
                                         }};

    auto counts = FailureCounts{};
    auto executor = ScanPlanExecutor{ml, std::move(plan), std::move(progress), counts.callback()};
    auto runResult = executor.run();
    REQUIRE(runResult);

    REQUIRE(runResult->processedIds.size() == 1);
    CHECK(runResult->processedIds.front() == originalTrackId);
    CHECK(runResult->relinkedCount == 1);
    CHECK(runResult->failureCount == 0);
    CHECK(counts.failed == 0);
    CHECK(sawFingerprinting);

    auto txn = ml.readTransaction();
    auto trackReader = ml.tracks().reader(txn);
    auto const optView = trackReader.get(originalTrackId, TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);

    auto const spec = trackSpecFromView(ml, *optView);
    CHECK(spec.uri == "renamed/song.flac");
    CHECK(spec.title == "Curated Title");
    CHECK(hasTag(spec, "favorite"));
    CHECK(hasCustomMetadata(spec, "catalog", "AOB-42"));

    auto manifestReader = ml.manifest().reader(txn);
    auto const oldManifestResult = manifestReader.get("song.flac");
    REQUIRE_FALSE(oldManifestResult);
    CHECK(oldManifestResult.error().code == Error::Code::NotFound);

    auto const newManifestResult = manifestReader.get("renamed/song.flac");
    REQUIRE(newManifestResult);
    CHECK(newManifestResult->trackId() == originalTrackId);
    CHECK(newManifestResult->status() == FileStatus::Available);
    CHECK(newManifestResult->audioPayloadLength() == originalPayloadLength);
    CHECK(newManifestResult->audioSignature() == originalSignature);

    auto const optManualList = ml.lists().reader(txn).get(manualListId);
    REQUIRE(optManualList);
    REQUIRE(optManualList->tracks().size() == 1);
    CHECK(optManualList->tracks()[0] == originalTrackId);
  }

  TEST_CASE("ScanPlanExecutor - rejects moved files whose live identity changed after planning",
            "[library][unit][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const originalFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, originalFile);

    auto ml = MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};

    auto originalTrackId = kInvalidTrackId;

    {
      auto scanner = LibraryScanner{ml};
      auto plan = scanner.buildPlan().value();
      auto executor = ScanPlanExecutor{ml, std::move(plan), nullptr, nullptr};
      auto runResult = executor.run();
      REQUIRE(runResult);
      REQUIRE(runResult->processedIds.size() == 1);
      originalTrackId = runResult->processedIds.front();
    }

    auto const movedFile = musicRoot / "renamed.flac";
    std::filesystem::rename(originalFile, movedFile);

    auto scanner = LibraryScanner{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.items.size() == 1);
    REQUIRE(plan.items.front().classification == ScanClassification::Moved);

    auto const differentAudio = audio::test::requireAudioFixture("hires.flac");
    std::filesystem::copy_file(differentAudio, movedFile, std::filesystem::copy_options::overwrite_existing);

    auto counts = FailureCounts{};
    auto executor = ScanPlanExecutor{ml, std::move(plan), nullptr, counts.callback()};
    auto runResult = executor.run();
    REQUIRE(runResult);

    CHECK(runResult->processedIds.empty());
    CHECK(runResult->relinkedCount == 0);
    CHECK(runResult->failureCount == 1);
    CHECK(counts.failed == 1);

    auto txn = ml.readTransaction();
    auto const optView = ml.tracks().reader(txn).get(originalTrackId, TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);
    CHECK(optView->property().uri() == "song.flac");

    auto manifestReader = ml.manifest().reader(txn);
    CHECK(manifestReader.get("song.flac"));
    auto const newManifestResult = manifestReader.get("renamed.flac");
    REQUIRE_FALSE(newManifestResult);
    CHECK(newManifestResult.error().code == Error::Code::NotFound);
  }

  TEST_CASE("ScanPlanExecutor - aborts the transaction when moved manifest removal fails", "[library][unit][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const originalFile = musicRoot / "song.flac";
    auto const movedFile = musicRoot / "renamed.flac";
    std::filesystem::copy_file(sourceFile, originalFile);

    auto ml = MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};

    auto originalTrackId = kInvalidTrackId;

    {
      auto scanner = LibraryScanner{ml};
      auto plan = scanner.buildPlan().value();
      auto executor = ScanPlanExecutor{ml, std::move(plan), nullptr, nullptr};
      auto runResult = executor.run();
      REQUIRE(runResult);
      REQUIRE(runResult->processedIds.size() == 1);
      originalTrackId = runResult->processedIds.front();
    }

    std::filesystem::copy_file(sourceFile, movedFile);

    auto txn = ml.readTransaction();
    auto const originalManifestResult = ml.manifest().reader(txn).get("song.flac");
    REQUIRE(originalManifestResult);

    auto item = newAudioItem(movedFile, "renamed-again.flac");
    item.classification = ScanClassification::Moved;
    item.oldUri = std::string(501, 'x');
    item.trackId = originalTrackId;
    item.audioPayloadLength = originalManifestResult->audioPayloadLength();
    item.audioSignature = originalManifestResult->audioSignature();

    auto plan = ScanPlan{};
    plan.items.push_back(std::move(item));

    auto counts = FailureCounts{};
    auto executor = ScanPlanExecutor{ml, std::move(plan), nullptr, counts.callback()};
    auto runResult = executor.run();
    REQUIRE(runResult);

    CHECK(runResult->processedIds.empty());
    CHECK(runResult->relinkedCount == 0);
    CHECK(runResult->failureCount == 1);
    CHECK(counts.failed == 1);

    auto verifyTxn = ml.readTransaction();
    auto const optView = ml.tracks().reader(verifyTxn).get(originalTrackId, TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);
    CHECK(optView->property().uri() == "song.flac");

    auto manifestReader = ml.manifest().reader(verifyTxn);
    CHECK(manifestReader.get("song.flac"));
    auto const newManifestResult = manifestReader.get("renamed-again.flac");
    REQUIRE_FALSE(newManifestResult);
    CHECK(newManifestResult.error().code == Error::Code::NotFound);
  }

  TEST_CASE("ScanPlanExecutor - reports corrupted file failures", "[library][unit][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const corruptedFile = musicRoot / "corrupted.flac";
    {
      auto out = std::ofstream{corruptedFile, std::ios::binary};
      out << "NOT A FLAC FILE";
    }

    auto ml = MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};

    auto scanner = LibraryScanner{ml};
    auto plan = scanner.buildPlan().value();

    auto counts = FailureCounts{};
    auto executor = ScanPlanExecutor{ml, std::move(plan), nullptr, counts.callback()};
    auto runResult = executor.run();
    REQUIRE(runResult);

    auto const& result = *runResult;
    CHECK(counts.failed == 1);
    CHECK(result.failureCount == 1);
    CHECK(result.processedIds.empty());
  }

  TEST_CASE("ScanPlanExecutor - propagates unexpected process exceptions", "[library][unit][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto ml = MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};

    auto plan = ScanPlan{};
    plan.items.push_back(ScanItem{.uri = "bad.flac", .oldUri = {}, .fullPath = musicRoot / "bad.flac"});

    auto counts = FailureCounts{};
    auto thrower = [](ScanPlanExecutor::Progress const&) { throwUnexpectedProgressFailure(); };
    auto executor = ScanPlanExecutor{ml, std::move(plan), std::move(thrower), counts.callback()};

    REQUIRE_THROWS_AS(std::ignore = executor.run(), std::runtime_error);
    CHECK(counts.failed == 0);
  }

  TEST_CASE("ScanPlanExecutor - ignores non-decodable files omitted from the plan", "[library][unit][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    // A text file, plus audio formats we have no reader for. The scanner only
    // admits decodable extensions, so none of these reach the executor.
    for (auto const* const name : {"notes.txt", "cover.jpg", "song.wav", "song.ogg", "song.alac"})
    {
      auto out = std::ofstream{musicRoot / name, std::ios::binary};
      out << "not a supported audio file";
    }

    auto ml = MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};

    auto scanner = LibraryScanner{ml};
    auto plan = scanner.buildPlan().value();
    CHECK(plan.items.empty());

    auto counts = FailureCounts{};
    auto executor = ScanPlanExecutor{ml, std::move(plan), nullptr, counts.callback()};
    auto runResult = executor.run();
    REQUIRE(runResult);

    CHECK(runResult->processedIds.empty());
    CHECK(runResult->failureCount == 0);
    CHECK(counts.failed == 0);
  }
} // namespace ao::library::test
