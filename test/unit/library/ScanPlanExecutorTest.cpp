// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ao/library/ScanPlanExecutor.h"

#include "ao/Type.h"
#include "ao/library/FileManifestLayout.h"
#include "ao/library/FileManifestStore.h"
#include "ao/library/LibraryScanner.h"
#include "ao/library/MusicLibrary.h"
#include "ao/library/TrackStore.h"
#include "ao/library/TrackView.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <utility>

namespace ao::library::test
{
  namespace
  {
    struct TempDir final
    {
      std::filesystem::path path;
      TempDir()
      {
        path = std::filesystem::temp_directory_path() / "aobus_scan_exec_test";
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
      }
      ~TempDir() { std::filesystem::remove_all(path); }

      TempDir(TempDir const&) = delete;
      TempDir& operator=(TempDir const&) = delete;
      TempDir(TempDir&&) = delete;
      TempDir& operator=(TempDir&&) = delete;
    };
  }

  TEST_CASE("ScanPlanExecutor Full Cycle", "[core][library][scan]")
  {
    auto const temp = TempDir{};
    auto const musicRoot = temp.path / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "basic_metadata.flac";
    auto const targetFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);

    auto ml = MusicLibrary{musicRoot, temp.path / "db"};

    // 1. Initial Scan and Execution
    {
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

    // 2. Unchanged Scan
    {
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

    // 3. Changed Scan (Hot Update)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds{100});
      {
        auto out = std::ofstream{targetFile, std::ios::binary | std::ios::app};
        out << "some extra garbage";
      }
      auto const now = std::filesystem::file_time_type::clock::now();
      std::filesystem::last_write_time(targetFile, now);

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

    // 4. Missing Scan
    {
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
  }

  TEST_CASE("ScanPlanExecutor Error Handling", "[core][library][scan]")
  {
    auto const temp = TempDir{};
    auto const musicRoot = temp.path / "music";
    std::filesystem::create_directories(musicRoot);

    auto const corruptedFile = musicRoot / "corrupted.flac";
    {
      auto out = std::ofstream{corruptedFile, std::ios::binary};
      out << "NOT A FLAC FILE";
    }

    auto ml = MusicLibrary{musicRoot, temp.path / "db"};

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
