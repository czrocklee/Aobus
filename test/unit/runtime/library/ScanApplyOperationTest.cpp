// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/utility/Hash128.h>
#include <runtime/library/ScanApplyOperation.h>
#include <runtime/library/ScanPlanBuilder.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
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

    ScanItem makeNewAudioScanItem(std::filesystem::path const& fullPath, std::string_view uri)
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

  TEST_CASE("ScanApplyOperation - initial scans process new files", "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const targetFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");

    auto scanner = ScanPlanBuilder{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items[0].classification == ScanClassification::New);

    auto counts = FailureCounts{};
    auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, counts.callback()};
    auto runResult = executor.run();
    REQUIRE(runResult);

    auto const& result = *runResult;
    CHECK(result.processedIds.size() == 1);
    CHECK(result.failureCount == 0);
    CHECK(counts.failed == 0);

    auto transaction = ml.readTransaction();
    auto const optView = ml.tracks().reader(transaction).get(result.processedIds[0]);
    REQUIRE(optView);
    CHECK(optView->metadata().title() == "Test Title");

    auto const manifestResult = ml.manifest().reader(transaction).get("song.flac");
    REQUIRE(manifestResult);
    CHECK(manifestResult->audioPayloadLength() > 0);
    CHECK(manifestResult->audioSignature() != utility::Hash128{});
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

    auto scanner = ScanPlanBuilder{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items[0].classification == ScanClassification::New);

    auto counts = FailureCounts{};
    auto executor = ScanApplyOperation{ml,
                                       std::move(plan),
                                       nullptr,
                                       counts.callback(),
                                       ScanApplyOptions{.audioIdentityPolicy = AudioIdentityPolicy::DeferNew}};
    auto runResult = executor.run();
    REQUIRE(runResult);

    CHECK(runResult->processedIds.size() == 1);
    CHECK(runResult->failureCount == 0);
    CHECK(counts.failed == 0);

    auto transaction = ml.readTransaction();
    auto const manifestResult = ml.manifest().reader(transaction).get("song.flac");
    REQUIRE(manifestResult);
    CHECK(manifestResult->status() == library::FileStatus::Available);
    CHECK(manifestResult->fileSize() == std::filesystem::file_size(targetFile));
    CHECK(manifestResult->mtime() == fileMtime(targetFile));
    CHECK(manifestResult->audioPayloadLength() == 0);
    CHECK(manifestResult->audioSignature() == utility::Hash128{});
  }

  TEST_CASE("ScanApplyOperation - defer policy still uses cached new identity", "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const targetFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);

    auto cachedSignature = utility::Hash128{};
    cachedSignature.bytes[15] = std::byte{0x42};

    auto plan = ScanPlan{};
    auto item = makeNewAudioScanItem(targetFile, "song.flac");
    item.audioPayloadLength = 12345;
    item.audioSignature = cachedSignature;
    plan.items.push_back(std::move(item));

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    auto counts = FailureCounts{};
    auto executor = ScanApplyOperation{ml,
                                       std::move(plan),
                                       nullptr,
                                       counts.callback(),
                                       ScanApplyOptions{.audioIdentityPolicy = AudioIdentityPolicy::DeferNew}};
    auto runResult = executor.run();
    REQUIRE(runResult);

    CHECK(runResult->processedIds.size() == 1);
    CHECK(runResult->failureCount == 0);
    CHECK(counts.failed == 0);

    auto transaction = ml.readTransaction();
    auto const manifestResult = ml.manifest().reader(transaction).get("song.flac");
    REQUIRE(manifestResult);
    CHECK(manifestResult->audioPayloadLength() == 12345);
    CHECK(manifestResult->audioSignature() == cachedSignature);
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

    auto scanner = ScanPlanBuilder{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.items.size() == 1);

    auto progressEvents = std::vector<ScanApplyProgress>{};
    auto progress = std::move_only_function<void(ScanApplyProgress const&)>{
      [&progressEvents](ScanApplyProgress const& progress) { progressEvents.push_back(progress); }};

    auto counts = FailureCounts{};
    auto executor = ScanApplyOperation{ml, std::move(plan), std::move(progress), counts.callback()};
    auto runResult = executor.run();
    REQUIRE(runResult);

    CHECK(runResult->processedIds.size() == 1);
    CHECK(runResult->failureCount == 0);
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

    auto scanner = ScanPlanBuilder{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.items.size() == 2);
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

    auto scanner = ScanPlanBuilder{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.items.size() == 1);

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
    auto runResult = executor.run(stopSource.get_token());
    REQUIRE(runResult);

    CHECK(runResult->cancelled);
    CHECK(runResult->processedIds.empty());
    CHECK(runResult->failureCount == 0);
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
    auto const targetFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");

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
    plan.items.push_back(makeNewAudioScanItem(targetFile, "song.flac"));

    auto stopSource = std::stop_source{};
    auto progress = std::move_only_function<void(ScanApplyProgress const&)>{
      [&stopSource](ScanApplyProgress const& progress)
      {
        if (progress.stage == ScanApplyProgressStage::Fingerprinting)
        {
          stopSource.request_stop();
        }
      }};

    auto counts = FailureCounts{};
    auto executor = ScanApplyOperation{ml, std::move(plan), std::move(progress), counts.callback()};
    auto runResult = executor.run(stopSource.get_token());
    REQUIRE(runResult);

    CHECK(runResult->cancelled);
    CHECK(runResult->processedIds.empty());
    CHECK(runResult->failureCount == 0);
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
      auto scanner = ScanPlanBuilder{ml};
      auto plan = scanner.buildPlan().value();
      auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, nullptr};
      auto runResult = executor.run();
      REQUIRE(runResult);
    }

    // Second scan should find unchanged file
    auto scanner = ScanPlanBuilder{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items[0].classification == ScanClassification::Unchanged);

    auto counts = FailureCounts{};
    auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, counts.callback()};
    auto runResult = executor.run();
    REQUIRE(runResult);

    auto const& result = *runResult;
    // An unchanged file is skipped silently: nothing processed, nothing reported.
    CHECK(result.processedIds.empty());
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
      auto scanner = ScanPlanBuilder{ml};
      auto plan = scanner.buildPlan().value();
      auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, nullptr};
      auto runResult = executor.run();
      REQUIRE(runResult);
    }

    std::uint64_t oldPayloadLength = 0;
    auto oldSignature = utility::Hash128{};
    {
      auto transaction = ml.readTransaction();
      auto const manifestResult = ml.manifest().reader(transaction).get("song.flac");
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

    auto scanner = ScanPlanBuilder{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items[0].classification == ScanClassification::Changed);

    auto counts = FailureCounts{};
    auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, counts.callback()};
    auto runResult = executor.run();
    REQUIRE(runResult);

    auto const& result = *runResult;
    CHECK(result.processedIds.size() == 1);
    CHECK(result.failureCount == 0);
    CHECK(counts.failed == 0);

    auto transaction = ml.readTransaction();
    auto const manifestResult = ml.manifest().reader(transaction).get("song.flac");
    REQUIRE(manifestResult);
    auto const actualMtime =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::filesystem::last_write_time(targetFile).time_since_epoch())
                                   .count());
    CHECK(manifestResult->mtime() == actualMtime);
    CHECK(manifestResult->audioPayloadLength() > oldPayloadLength);
    CHECK(manifestResult->audioSignature() != oldSignature);
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
      auto scanner = ScanPlanBuilder{ml};
      auto plan = scanner.buildPlan().value();
      auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, nullptr};
      auto runResult = executor.run();
      REQUIRE(runResult);
    }

    // Remove the file
    std::filesystem::remove(targetFile);

    auto scanner = ScanPlanBuilder{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items[0].classification == ScanClassification::Missing);

    auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, nullptr};
    auto runResult = executor.run();
    REQUIRE(runResult);

    auto transaction = ml.readTransaction();
    auto const manifestResult = ml.manifest().reader(transaction).get("song.flac");
    REQUIRE(manifestResult);
    CHECK(manifestResult->status() == library::FileStatus::Missing);
    CHECK(runResult->processedIds.empty());
    CHECK(runResult->missingCount == 1);
    CHECK(runResult->failureCount == 0);
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
      auto scanner = ScanPlanBuilder{ml};
      auto plan = scanner.buildPlan().value();
      auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, nullptr};
      auto runResult = executor.run();
      REQUIRE(runResult);
      REQUIRE(runResult->processedIds.size() == 1);
      originalTrackId = runResult->processedIds.front();
    }

    std::uint64_t originalPayloadLength = 0;
    auto originalSignature = utility::Hash128{};
    {
      auto transaction = ml.readTransaction();
      auto const manifestResult = ml.manifest().reader(transaction).get("song.flac");
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
      auto transaction = ml.writeTransaction();
      auto listBuilder = library::ListBuilder::makeEmpty();
      listBuilder.name("Manual").tracks().add(originalTrackId);
      auto createResult = ml.lists().writer(transaction).create(listBuilder.serialize());
      REQUIRE(createResult);
      manualListId = createResult->first;
      REQUIRE(transaction.commit());
    }

    auto const movedFile = musicRoot / "renamed" / "song.flac";
    std::filesystem::create_directories(movedFile.parent_path());
    std::filesystem::rename(originalFile, movedFile);

    auto scanner = ScanPlanBuilder{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items.front().classification == ScanClassification::Moved);
    CHECK(plan.items.front().oldUri == "song.flac");
    CHECK(plan.items.front().trackId == originalTrackId);

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
    auto executor = ScanApplyOperation{ml, std::move(plan), std::move(progress), counts.callback()};
    auto runResult = executor.run();
    REQUIRE(runResult);

    REQUIRE(runResult->processedIds.size() == 1);
    CHECK(runResult->processedIds.front() == originalTrackId);
    CHECK(runResult->relinkedCount == 1);
    CHECK(runResult->failureCount == 0);
    CHECK(counts.failed == 0);
    CHECK(sawFingerprinting);

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
    auto const oldManifestResult = manifestReader.get("song.flac");
    REQUIRE_FALSE(oldManifestResult);
    CHECK(oldManifestResult.error().code == Error::Code::NotFound);

    auto const newManifestResult = manifestReader.get("renamed/song.flac");
    REQUIRE(newManifestResult);
    CHECK(newManifestResult->trackId() == originalTrackId);
    CHECK(newManifestResult->status() == library::FileStatus::Available);
    CHECK(newManifestResult->audioPayloadLength() == originalPayloadLength);
    CHECK(newManifestResult->audioSignature() == originalSignature);

    auto const optManualList = ml.lists().reader(transaction).get(manualListId);
    REQUIRE(optManualList);
    REQUIRE(optManualList->tracks().size() == 1);
    CHECK(optManualList->tracks()[0] == originalTrackId);
  }

  TEST_CASE("ScanApplyOperation - rejects moved files whose live identity changed after planning",
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
      auto scanner = ScanPlanBuilder{ml};
      auto plan = scanner.buildPlan().value();
      auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, nullptr};
      auto runResult = executor.run();
      REQUIRE(runResult);
      REQUIRE(runResult->processedIds.size() == 1);
      originalTrackId = runResult->processedIds.front();
    }

    auto const movedFile = musicRoot / "renamed.flac";
    std::filesystem::rename(originalFile, movedFile);

    auto scanner = ScanPlanBuilder{ml};
    auto plan = scanner.buildPlan().value();
    REQUIRE(plan.items.size() == 1);
    REQUIRE(plan.items.front().classification == ScanClassification::Moved);

    auto const differentAudio = audio::test::requireAudioFixture("hires.flac");
    std::filesystem::copy_file(differentAudio, movedFile, std::filesystem::copy_options::overwrite_existing);

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
    auto runResult = executor.run();
    REQUIRE(runResult);

    CHECK(runResult->processedIds.empty());
    CHECK(runResult->relinkedCount == 0);
    CHECK(runResult->failureCount == 1);
    CHECK(counts.failed == 1);
    CHECK(sawFingerprinting);

    auto transaction = ml.readTransaction();
    auto const optView =
      ml.tracks().reader(transaction).get(originalTrackId, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);
    CHECK(optView->property().uri() == "song.flac");

    auto manifestReader = ml.manifest().reader(transaction);
    CHECK(manifestReader.get("song.flac"));
    auto const newManifestResult = manifestReader.get("renamed.flac");
    REQUIRE_FALSE(newManifestResult);
    CHECK(newManifestResult.error().code == Error::Code::NotFound);
  }

  TEST_CASE("ScanApplyOperation - aborts the transaction when moved manifest removal fails",
            "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const originalFile = musicRoot / "song.flac";
    auto const movedFile = musicRoot / "renamed.flac";
    std::filesystem::copy_file(sourceFile, originalFile);

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");

    auto originalTrackId = kInvalidTrackId;

    {
      auto scanner = ScanPlanBuilder{ml};
      auto plan = scanner.buildPlan().value();
      auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, nullptr};
      auto runResult = executor.run();
      REQUIRE(runResult);
      REQUIRE(runResult->processedIds.size() == 1);
      originalTrackId = runResult->processedIds.front();
    }

    std::filesystem::copy_file(sourceFile, movedFile);

    auto transaction = ml.readTransaction();
    auto const originalManifestResult = ml.manifest().reader(transaction).get("song.flac");
    REQUIRE(originalManifestResult);

    auto item = makeNewAudioScanItem(movedFile, "renamed-again.flac");
    item.classification = ScanClassification::Moved;
    item.oldUri = std::string(501, 'x');
    item.trackId = originalTrackId;
    item.audioPayloadLength = originalManifestResult->audioPayloadLength();
    item.audioSignature = originalManifestResult->audioSignature();

    auto plan = ScanPlan{};
    plan.items.push_back(std::move(item));

    auto counts = FailureCounts{};
    auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, counts.callback()};
    auto runResult = executor.run();
    REQUIRE(runResult);

    CHECK(runResult->processedIds.empty());
    CHECK(runResult->relinkedCount == 0);
    CHECK(runResult->failureCount == 1);
    CHECK(counts.failed == 1);

    auto verifyTransaction = ml.readTransaction();
    auto const optView =
      ml.tracks().reader(verifyTransaction).get(originalTrackId, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);
    CHECK(optView->property().uri() == "song.flac");

    auto manifestReader = ml.manifest().reader(verifyTransaction);
    CHECK(manifestReader.get("song.flac"));
    auto const newManifestResult = manifestReader.get("renamed-again.flac");
    REQUIRE_FALSE(newManifestResult);
    CHECK(newManifestResult.error().code == Error::Code::NotFound);
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

    auto scanner = ScanPlanBuilder{ml};
    auto plan = scanner.buildPlan().value();

    auto counts = FailureCounts{};
    auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, counts.callback()};
    auto runResult = executor.run();
    REQUIRE(runResult);

    auto const& result = *runResult;
    CHECK(counts.failed == 1);
    CHECK(result.failureCount == 1);
    CHECK(result.processedIds.empty());
  }

  TEST_CASE("ScanApplyOperation - propagates unexpected process exceptions", "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");

    auto plan = ScanPlan{};
    plan.items.push_back(ScanItem{.uri = "bad.flac", .oldUri = {}, .fullPath = musicRoot / "bad.flac"});

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

    auto scanner = ScanPlanBuilder{ml};
    auto plan = scanner.buildPlan().value();
    CHECK(plan.items.empty());

    auto counts = FailureCounts{};
    auto executor = ScanApplyOperation{ml, std::move(plan), nullptr, counts.callback()};
    auto runResult = executor.run();
    REQUIRE(runResult);

    CHECK(runResult->processedIds.empty());
    CHECK(runResult->failureCount == 0);
    CHECK(counts.failed == 0);
  }
} // namespace ao::rt::test
