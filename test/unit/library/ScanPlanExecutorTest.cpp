// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/lmdb/TestUtils.h"
#include <ao/Type.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryScanner.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ScanPlanExecutor.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <utility>

namespace ao::library::test
{
  using namespace ao::lmdb::test;

  TEST_CASE("ScanPlanExecutor - Initial scan processes new files", "[library][unit][scan]")
  {
    auto const temp = TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "basic_metadata.flac";
    auto const targetFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);

    auto ml = MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};

    auto scanner = LibraryScanner{ml};
    auto plan = scanner.buildPlan();
    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items[0].classification == ScanClassification::New);

    auto executor = ScanPlanExecutor{ml, std::move(plan), nullptr, nullptr};
    executor.run();

    auto const result = executor.result();
    CHECK(result.processedIds.size() == 1);
    CHECK(result.failureCount == 0);
    CHECK(result.skippedCount == 0);

    auto txn = ml.readTransaction();
    auto const optView = ml.tracks().reader(txn).get(result.processedIds[0]);
    REQUIRE(optView);
    CHECK(optView->metadata().title() == "Test Title");
  }

  TEST_CASE("ScanPlanExecutor - Unchanged files are skipped", "[library][unit][scan]")
  {
    auto const temp = TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "basic_metadata.flac";
    auto const targetFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);

    auto ml = MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};

    // First scan to populate the manifest
    {
      auto scanner = LibraryScanner{ml};
      auto plan = scanner.buildPlan();
      auto executor = ScanPlanExecutor{ml, std::move(plan), nullptr, nullptr};
      executor.run();
    }

    // Second scan should find unchanged file
    auto scanner = LibraryScanner{ml};
    auto plan = scanner.buildPlan();
    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items[0].classification == ScanClassification::Unchanged);

    auto executor = ScanPlanExecutor{ml, std::move(plan), nullptr, nullptr};
    executor.run();

    auto const result = executor.result();
    CHECK(result.processedIds.empty());
    CHECK(result.skippedCount == 1);
  }

  TEST_CASE("ScanPlanExecutor - Changed files trigger hot update", "[library][unit][scan]")
  {
    auto const temp = TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "basic_metadata.flac";
    auto const targetFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);

    auto ml = MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};

    // First scan to populate the manifest
    {
      auto scanner = LibraryScanner{ml};
      auto plan = scanner.buildPlan();
      auto executor = ScanPlanExecutor{ml, std::move(plan), nullptr, nullptr};
      executor.run();
    }

    // Modify targetFile and advance mtime
    auto const oldMtime = std::filesystem::last_write_time(targetFile);
    {
      auto out = std::ofstream{targetFile, std::ios::binary | std::ios::app};
      out << "some extra garbage";
    }
    std::filesystem::last_write_time(targetFile, oldMtime + std::chrono::seconds{10});

    auto scanner = LibraryScanner{ml};
    auto plan = scanner.buildPlan();
    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items[0].classification == ScanClassification::Changed);

    auto executor = ScanPlanExecutor{ml, std::move(plan), nullptr, nullptr};
    executor.run();

    auto const result = executor.result();
    CHECK(result.processedIds.size() == 1);
    CHECK(result.failureCount == 0);

    auto txn = ml.readTransaction();
    auto const optManifest = ml.manifest().reader(txn).get("song.flac");
    REQUIRE(optManifest);
    auto const actualMtime =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::filesystem::last_write_time(targetFile).time_since_epoch())
                                   .count());
    CHECK(optManifest->mtime() == actualMtime);
  }

  TEST_CASE("ScanPlanExecutor - Missing files update manifest status", "[library][unit][scan]")
  {
    auto const temp = TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "basic_metadata.flac";
    auto const targetFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);

    auto ml = MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};

    // First scan to populate the manifest
    {
      auto scanner = LibraryScanner{ml};
      auto plan = scanner.buildPlan();
      auto executor = ScanPlanExecutor{ml, std::move(plan), nullptr, nullptr};
      executor.run();
    }

    // Remove the file
    std::filesystem::remove(targetFile);

    auto scanner = LibraryScanner{ml};
    auto plan = scanner.buildPlan();
    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items[0].classification == ScanClassification::Missing);

    auto executor = ScanPlanExecutor{ml, std::move(plan), nullptr, nullptr};
    executor.run();

    auto txn = ml.readTransaction();
    auto const optManifest = ml.manifest().reader(txn).get("song.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->status() == FileStatus::Missing);
  }

  TEST_CASE("ScanPlanExecutor - Error handling for corrupted files", "[library][unit][scan]")
  {
    auto const temp = TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const corruptedFile = musicRoot / "corrupted.flac";
    {
      auto out = std::ofstream{corruptedFile, std::ios::binary};
      out << "NOT A FLAC FILE";
    }

    auto ml = MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};

    auto scanner = LibraryScanner{ml};
    auto plan = scanner.buildPlan();

    auto executor = ScanPlanExecutor{ml, std::move(plan), nullptr, nullptr};
    executor.run();

    auto const result = executor.result();
    // It should either be skipped (if open returns null) or failure (if loadTrack throws)
    // Currently TagFile::open for garbage flac returns null, so it increments skippedCount.
    CHECK(result.skippedCount + result.failureCount == 1);
    CHECK(result.processedIds.empty());
  }
} // namespace ao::library::test
