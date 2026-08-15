// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "MusicLibraryTestSupport.h"

#include "test/unit/TestFixtureSupport.h"
#include <ao/Error.h>
#include <ao/library/MusicLibrary.h>

#include <filesystem>
#include <utility>

namespace ao::library::test
{
  MusicLibrary makeTestMusicLibrary(std::filesystem::path musicRoot, std::filesystem::path databasePath)
  {
    return ao::test::requireValue(
      MusicLibrary::open(std::move(musicRoot),
                         std::move(databasePath),
                         MusicLibrary::Options{.pinnedMapBytes = kTestMusicLibraryMapBytes}));
  }

  Result<MusicLibrary> openTestMusicLibrary(std::filesystem::path musicRoot, std::filesystem::path databasePath)
  {
    return MusicLibrary::open(std::move(musicRoot),
                              std::move(databasePath),
                              MusicLibrary::Options{.pinnedMapBytes = kTestMusicLibraryMapBytes});
  }
} // namespace ao::library::test
