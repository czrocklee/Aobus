// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ao/library/LibraryScanner.h"

#include "ao/library/FileManifestBuilder.h"
#include "ao/library/FileManifestStore.h"
#include "ao/library/MusicLibrary.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace ao::library::test
{
  namespace
  {
    struct TempDir final
    {
      std::filesystem::path path;
      TempDir()
      {
        path = std::filesystem::temp_directory_path() / "aobus_scanner_test";
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
      }
      ~TempDir() { std::filesystem::remove_all(path); }

      TempDir(TempDir const&) = delete;
      TempDir& operator=(TempDir const&) = delete;
      TempDir(TempDir&&) = delete;
      TempDir& operator=(TempDir&&) = delete;
    };

    void createFile(std::filesystem::path const& path)
    {
      auto f = std::ofstream{path};
      f << "dummy";
    }
  }

  TEST_CASE("LibraryScanner Classification", "[core][library][scan]")
  {
    auto const temp = TempDir{};
    auto const root = temp.path;
    auto const musicRoot = root / "music";
    std::filesystem::create_directories(musicRoot);

    createFile(musicRoot / "new.flac");
    createFile(musicRoot / "unchanged.mp3");
    createFile(musicRoot / "changed.wav");
    createFile(musicRoot / "unsupported.txt");

    auto ml = MusicLibrary{musicRoot, root / "db"};

    // Setup manifest for existing files
    {
      auto txn = ml.writeTransaction();
      auto manifestWriter = ml.manifest().writer(txn);

      // Unchanged
      char const* const unchangedUri = "unchanged.mp3";
      auto builder1 = FileManifestBuilder::createNew();
      builder1.trackId(TrackId{1})
        .fileSize(std::filesystem::file_size(musicRoot / unchangedUri))
        .mtime(std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::filesystem::last_write_time(musicRoot / unchangedUri).time_since_epoch())
                 .count());
      manifestWriter.put(unchangedUri, builder1.serialize());

      // Changed (different size)
      char const* const changedUri = "changed.wav";
      auto builder2 = FileManifestBuilder::createNew();
      builder2.trackId(TrackId{2}).fileSize(99999).mtime(0);
      manifestWriter.put(changedUri, builder2.serialize());

      // Missing (in manifest but not on disk)
      char const* const missingUri = "missing.flac";
      auto builder3 = FileManifestBuilder::createNew();
      builder3.trackId(TrackId{3});
      manifestWriter.put(missingUri, builder3.serialize());

      txn.commit();
    }

    auto scanner = LibraryScanner{ml};
    auto const plan = scanner.buildPlan();

    CHECK(plan.count(ScanClassification::New) == 1);
    CHECK(plan.count(ScanClassification::Unchanged) == 1);
    CHECK(plan.count(ScanClassification::Changed) == 1);
    CHECK(plan.count(ScanClassification::Missing) == 1);
    CHECK(plan.count(ScanClassification::Unsupported) == 1);

    // Verify specific items
    bool foundMissing = false;

    for (auto const& item : plan.items)
    {
      if (item.uri == "missing.flac")
      {
        CHECK(item.classification == ScanClassification::Missing);
        CHECK(item.trackId == TrackId{3});
        foundMissing = true;
      }
    }

    CHECK(foundMissing);
  }
} // namespace ao::library::test
