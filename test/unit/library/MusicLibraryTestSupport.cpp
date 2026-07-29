// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "MusicLibraryTestSupport.h"

#include <ao/Error.h>
#include <ao/library/MusicLibrary.h>

#include <filesystem>
#include <utility>

namespace ao::library::test
{
  MusicLibrary makeTestMusicLibrary(std::filesystem::path musicRoot, std::filesystem::path databasePath)
  {
    return MusicLibrary{
      std::move(musicRoot), std::move(databasePath), MusicLibrary::Options{.mapSize = kTestMusicLibraryMapSize}};
  }

  Result<MusicLibrary> openTestMusicLibrary(std::filesystem::path musicRoot, std::filesystem::path databasePath)
  {
    return MusicLibrary::open(
      std::move(musicRoot), std::move(databasePath), MusicLibrary::Options{.mapSize = kTestMusicLibraryMapSize});
  }
} // namespace ao::library::test
