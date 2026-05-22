// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ao/library/ImportWorker.h"

#include "ao/Type.h"
#include "ao/library/FileManifestBuilder.h"
#include "ao/library/FileManifestStore.h"
#include "ao/library/MusicLibrary.h"
#include "ao/library/TrackBuilder.h"
#include "ao/library/TrackStore.h"
#include "ao/library/TrackView.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <thread>

namespace ao::library::test
{
  namespace
  {
    struct TempDir final
    {
      std::filesystem::path path;
      TempDir()
      {
        path = std::filesystem::temp_directory_path() / "aobus_import_test";
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
      }
      ~TempDir() { std::filesystem::remove_all(path); }

      TempDir(TempDir const&) = delete;
      TempDir& operator=(TempDir const&) = delete;
      TempDir(TempDir&&) = delete;
      TempDir& operator=(TempDir&&) = delete;
    };

    void createMockFlac(std::filesystem::path const& path)
    {
      // Just a minimal file to satisfy std::filesystem::exists
      auto f = std::ofstream{path, std::ios::binary};
      f << "fLaC" << std::string(100, 0);
    }
  }

  TEST_CASE("ImportWorker Full Import Cycle", "[core][library][import]")
  {
    auto const temp = TempDir{};
    auto const musicRoot = temp.path / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "basic_metadata.flac";
    auto const targetFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);

    auto ml = MusicLibrary{musicRoot, temp.path / "db"};

    // 1. Initial Import
    {
      auto worker = ImportWorker{ml, {targetFile}, nullptr, nullptr};
      worker.run();

      auto const result = worker.result();
      CHECK(result.insertedIds.size() == 1);
      CHECK(result.failureCount == 0);
      CHECK(result.skippedCount == 0);

      auto txn = ml.readTransaction();
      auto const optView = ml.tracks().reader(txn).get(result.insertedIds[0]);
      REQUIRE(optView);
      CHECK(optView->metadata().title() == "Test Title");
      CHECK(optView->property().durationMs() > 0);
    }
  }

  TEST_CASE("ImportWorker Hot Update", "[core][library][import]")
  {
    auto const temp = TempDir{};
    auto const musicRoot = temp.path / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "basic_metadata.flac";
    auto const targetFile = musicRoot / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);

    auto ml = MusicLibrary{musicRoot, temp.path / "db"};

    auto originalId = kInvalidTrackId;

    // 1. Initial Import
    {
      auto worker = ImportWorker{ml, {targetFile}, nullptr, nullptr};
      worker.run();
      originalId = worker.result().insertedIds.at(0);
    }

    // 2. Simulate file change (mtime + append garbage)
    {
      // Sleep a bit to ensure mtime definitely changes if the FS resolution is low
      std::this_thread::sleep_for(std::chrono::milliseconds{100});

      {
        auto out = std::ofstream{targetFile, std::ios::binary | std::ios::app};
        out << "some extra garbage";
      }

      auto const now = std::filesystem::file_time_type::clock::now();
      std::filesystem::last_write_time(targetFile, now);
    }

    // 3. Run ImportWorker again
    {
      auto worker = ImportWorker{ml, {targetFile}, nullptr, nullptr};
      worker.run();

      auto const result = worker.result();
      CHECK(result.skippedCount == 0);
      CHECK(result.insertedIds.empty()); // Should be updated, not inserted as new
      CHECK(result.failureCount == 0);

      // Verify mtime in manifest is updated
      auto txn = ml.readTransaction();
      auto const optManifest = ml.manifest().reader(txn).get("song.flac");
      REQUIRE(optManifest);
      CHECK(optManifest->trackId() == originalId);

      auto const actualMtime =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::filesystem::last_write_time(targetFile).time_since_epoch())
                                     .count());
      CHECK(optManifest->mtime() == actualMtime);
    }
  }

  TEST_CASE("ImportWorker Parsing Failure", "[core][library][import]")
  {
    auto const temp = TempDir{};
    auto const musicRoot = temp.path / "music";
    std::filesystem::create_directories(musicRoot);

    // Create a file with .flac extension but completely garbage content
    auto const targetFile = musicRoot / "corrupted.flac";
    {
      auto out = std::ofstream{targetFile, std::ios::binary};
      out << "NOT A FLAC FILE AT ALL";
    }

    auto ml = MusicLibrary{musicRoot, temp.path / "db"};

    // Note: TagFile::open for FLAC usually checks for "fLaC" magic.
    // If it doesn't find it, it might return nullptr or throw depending on implementation.
    // In ImportWorker, if open returns nullptr, it increments skippedCount.
    // To trigger failureCount, we need loadTrack() to throw.

    // However, let's verify what happens with garbage.
    {
      auto worker = ImportWorker{ml, {targetFile}, nullptr, nullptr};
      worker.run();

      auto const result = worker.result();
      // Current implementation of ImportWorker:
      // if (!tagFile) { skippedCount++; return; }
      // So if it's garbage, it will likely be skipped.
      CHECK(result.skippedCount + result.failureCount == 1);
    }
  }

  TEST_CASE("ImportWorker Fast Skip", "[core][library][import]")
  {
    auto const temp = TempDir{};
    auto const musicPath = temp.path / "song.flac";
    createMockFlac(musicPath);

    auto ml = MusicLibrary{temp.path, temp.path / "db"};

    auto const uri = std::filesystem::relative(musicPath, ml.rootPath()).string();
    std::cout << "Test URI: " << uri << '\n';

    {
      auto txn = ml.writeTransaction();
      auto& dict = ml.dictionary();

      auto builder = TrackBuilder::createNew();
      builder.property().uri(uri).durationMs(1000).bitrate(128000);
      builder.metadata().title("Original Title");

      auto const [preparedHot, preparedCold] = builder.prepare(txn, dict, ml.resources());
      auto const [trackId, view] =
        ml.tracks().writer(txn).createHotCold(preparedHot.size(),
                                              preparedCold.size(),
                                              [&](TrackId, std::span<std::byte> hot, std::span<std::byte> cold)
                                              {
                                                preparedHot.writeTo(hot);
                                                preparedCold.writeTo(cold);
                                              });

      auto manifestWriter = ml.manifest().writer(txn);
      auto manifestBuilder = FileManifestBuilder::createNew();
      manifestBuilder.trackId(trackId)
        .fileSize(std::filesystem::file_size(musicPath))
        .mtime(std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::filesystem::last_write_time(musicPath).time_since_epoch())
                 .count());
      manifestWriter.put(uri, manifestBuilder.serialize());
      txn.commit();
    }

    // 2. Run ImportWorker on the same file (should Skip)
    {
      auto worker = ImportWorker{ml, {musicPath}, nullptr, nullptr};
      worker.run();

      auto const result = worker.result();
      CHECK(result.skippedCount == 1);
      CHECK(result.insertedIds.empty());
    }
  }

  TEST_CASE("ImportWorker URI Normalization Edge Cases", "[core][library][import][uri]")
  {
    auto const temp = TempDir{};
    auto const musicPath = temp.path / "nested" / "dir" / "song.flac";
    std::filesystem::create_directories(musicPath.parent_path());
    createMockFlac(musicPath);

    auto ml = MusicLibrary{temp.path, temp.path / "db"};

    // We expect the internal URI to be standard generic with forward slashes
    auto const* const expectedUri = "nested/dir/song.flac";

    {
      auto txn = ml.writeTransaction();
      auto& dict = ml.dictionary();

      auto builder = TrackBuilder::createNew();
      builder.property().uri(expectedUri).durationMs(1000).bitrate(128000);
      builder.metadata().title("Original Title");

      auto const [preparedHot, preparedCold] = builder.prepare(txn, dict, ml.resources());
      auto const [trackId, view] =
        ml.tracks().writer(txn).createHotCold(preparedHot.size(),
                                              preparedCold.size(),
                                              [&](TrackId, std::span<std::byte> hot, std::span<std::byte> cold)
                                              {
                                                preparedHot.writeTo(hot);
                                                preparedCold.writeTo(cold);
                                              });

      auto manifestWriter = ml.manifest().writer(txn);
      auto manifestBuilder = FileManifestBuilder::createNew();
      manifestBuilder.trackId(trackId)
        .fileSize(std::filesystem::file_size(musicPath))
        .mtime(std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::filesystem::last_write_time(musicPath).time_since_epoch())
                 .count());
      manifestWriter.put(expectedUri, manifestBuilder.serialize());
      txn.commit();
    }

    {
      // The ImportWorker should resolve the musicPath to exactly expectedUri internally.
      // If it doesn't, it will fail to find it in the manifest and attempt to parse the mock FLAC,
      // resulting in a failure rather than a skip.
      auto worker = ImportWorker{ml, {musicPath}, nullptr, nullptr};
      worker.run();

      auto const result = worker.result();
      CHECK(result.skippedCount == 1);
      CHECK(result.failureCount == 0);
      CHECK(result.insertedIds.empty());
    }
  }
} // namespace ao::library::test
