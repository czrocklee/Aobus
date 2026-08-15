// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/library/MusicLibrary.h>

#include <cstdint>
#include <filesystem>

namespace ao::library::test
{
  inline constexpr std::uint64_t kTestMusicLibraryMapBytes = std::uint64_t{64} * 1024 * 1024;

  MusicLibrary makeTestMusicLibrary(std::filesystem::path musicRoot, std::filesystem::path databasePath);
  Result<MusicLibrary> openTestMusicLibrary(std::filesystem::path musicRoot, std::filesystem::path databasePath);
} // namespace ao::library::test
