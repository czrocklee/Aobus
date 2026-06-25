// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryScanner.h>
#include <ao/library/MusicLibrary.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace ao::library::test
{
  namespace
  {
    void createFile(std::filesystem::path const& path)
    {
      auto f = std::ofstream{path};
      f << "dummy";
    }
  }

  TEST_CASE("LibraryScanner Classification", "[library][unit][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const& root = temp.path();
    auto const musicRoot = std::filesystem::path{root} / "music";
    std::filesystem::create_directories(musicRoot);

    createFile(musicRoot / "new.flac");
    createFile(musicRoot / "unchanged.mp3");
    createFile(musicRoot / "changed.m4a");
    createFile(musicRoot / "unsupported.txt");

    auto ml = MusicLibrary{musicRoot, std::filesystem::path{root} / "db"};

    // Setup manifest for existing files
    {
      auto txn = ml.writeTransaction();
      auto manifestWriter = ml.manifest().writer(txn);

      // Unchanged
      char const* const unchangedUri = "unchanged.mp3";
      auto builder1 = FileManifestBuilder::createNew();
      builder1.trackId(TrackId{1})
        .fileSize(std::filesystem::file_size(musicRoot / unchangedUri))
        .mtime(
          static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::filesystem::last_write_time(musicRoot / unchangedUri).time_since_epoch())
                                       .count()));
      REQUIRE(manifestWriter.put(unchangedUri, builder1.serialize()));

      // Changed (different size)
      char const* const changedUri = "changed.m4a";
      auto builder2 = FileManifestBuilder::createNew();
      builder2.trackId(TrackId{2}).fileSize(99999).mtime(0);
      REQUIRE(manifestWriter.put(changedUri, builder2.serialize()));

      // Missing (in manifest but not on disk)
      char const* const missingUri = "missing.flac";
      auto builder3 = FileManifestBuilder::createNew();
      builder3.trackId(TrackId{3});
      REQUIRE(manifestWriter.put(missingUri, builder3.serialize()));

      REQUIRE(txn.commit());
    }

    auto scanner = LibraryScanner{ml};
    auto const plan = scanner.buildPlan().value();

    CHECK(plan.count(ScanClassification::New) == 1);
    CHECK(plan.count(ScanClassification::Unchanged) == 1);
    CHECK(plan.count(ScanClassification::Changed) == 1);
    CHECK(plan.count(ScanClassification::Missing) == 1);

    // The non-audio file is filtered at the walk: it never enters the plan.
    CHECK(plan.items.size() == 4);

    // Verify specific items
    bool foundMissing = false;

    for (auto const& item : plan.items)
    {
      CHECK(item.uri != "unsupported.txt");

      if (item.uri == "missing.flac")
      {
        CHECK(item.classification == ScanClassification::Missing);
        CHECK(item.trackId == TrackId{3});
        foundMissing = true;
      }
    }

    CHECK(foundMissing);
  }

  TEST_CASE("LibraryScanner IO Error Handling", "[library][unit][scan][error]")
  {
    auto const temp = ao::test::TempDir{};
    auto const& root = temp.path();
    auto const musicRoot = std::filesystem::path{root} / "music";
    std::filesystem::create_directories(musicRoot / "ok_dir");
    std::filesystem::create_directories(musicRoot / "restricted_dir");

    createFile(musicRoot / "ok_dir" / "song1.flac");
    createFile(musicRoot / "another.mp3");

    // Make restricted_dir inaccessible.
    // permissions() is a no-op when running as root, so skip in that case.
    std::filesystem::permissions(musicRoot / "restricted_dir", std::filesystem::perms::none);

    if (::geteuid() == 0)
    {
      SKIP("permissions test is meaningless when running as root");
    }

    auto ml = MusicLibrary{musicRoot, std::filesystem::path{root} / "db"};
    auto scanner = LibraryScanner{ml};
    auto const plan = scanner.buildPlan().value();

    // Reset permissions so ao::test::TempDir can clean up
    std::filesystem::permissions(musicRoot / "restricted_dir", std::filesystem::perms::owner_all);

    // We expect:
    // 1. ok_dir/song1.flac (New)
    // 2. another.mp3 (New)
    // 3. restricted_dir (Error)

    bool foundOk = false;
    bool foundAnother = false;
    bool foundRestricted = false;

    for (auto const& item : plan.items)
    {
      if (item.uri == "ok_dir/song1.flac")
      {
        CHECK(item.classification == ScanClassification::New);
        foundOk = true;
      }
      else if (item.uri == "another.mp3")
      {
        CHECK(item.classification == ScanClassification::New);
        foundAnother = true;
      }
      else if (item.uri == "restricted_dir")
      {
        CHECK(item.classification == ScanClassification::Error);
        foundRestricted = true;
      }
    }

    CHECK(foundOk);
    CHECK(foundAnother);
    CHECK(foundRestricted);
  }

  TEST_CASE("LibraryScanner Empty Root Boundary", "[library][unit][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "empty_music";
    std::filesystem::create_directories(musicRoot);

    auto ml = MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};
    auto scanner = LibraryScanner{ml};
    auto const plan = scanner.buildPlan().value();

    CHECK(plan.items.empty());
  }

  TEST_CASE("LibraryScanner Missing Root Is Fatal", "[library][unit][scan][error]")
  {
    auto const temp = ao::test::TempDir{};
    // Point the library at a music root that does not exist. The database still
    // lives under a real directory, so the library itself opens cleanly and only
    // the scan fails - distinguishing "cannot scan" from "scanned an empty root".
    auto const musicRoot = std::filesystem::path{temp.path()} / "does_not_exist";

    auto ml = MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};
    auto scanner = LibraryScanner{ml};
    auto const result = scanner.buildPlan();

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotFound);
  }

  TEST_CASE("LibraryScanner URI Canonization Edge Cases", "[library][unit][scan][uri]")
  {
    auto const temp = ao::test::TempDir{};
    auto const& root = temp.path();
    auto const musicRoot = std::filesystem::path{root} / "music";
    std::filesystem::create_directories(musicRoot / "nested" / "dir");

    createFile(musicRoot / "nested" / "dir" / "song.flac");

    auto ml = MusicLibrary{musicRoot, std::filesystem::path{root} / "db"};
    auto scanner = LibraryScanner{ml};
    auto const plan = scanner.buildPlan().value();

    REQUIRE(plan.items.size() == 1);

    // Verify that the computed URI is standard, generic, and uses forward slashes
    for (auto const& item : plan.items)
    {
      CHECK(item.uri == "nested/dir/song.flac");
      CHECK(item.uri.find('\\') == std::string::npos);
      CHECK(item.uri.find("./") == std::string::npos);
    }
  }
} // namespace ao::library::test
