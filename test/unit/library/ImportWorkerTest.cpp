// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ao/library/ImportWorker.h"

#include "ao/Type.h"
#include "ao/library/FileManifestBuilder.h"
#include "ao/library/FileManifestStore.h"
#include "ao/library/MusicLibrary.h"
#include "ao/library/TrackBuilder.h"
#include "ao/library/TrackStore.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>

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
} // namespace ao::library::test
