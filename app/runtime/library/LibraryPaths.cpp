// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/LibraryPaths.h>

#include <filesystem>
#include <utility>

namespace ao::rt
{
  namespace
  {
    constexpr auto kManagedDataDirectoryName = ".aobus";
    constexpr auto kDatabaseDirectoryName = "library";
    constexpr auto kLogsDirectoryName = "logs";
    constexpr auto kLmdbDataFileName = "data.mdb";
  } // namespace

  LibraryPaths::LibraryPaths(std::filesystem::path musicRoot)
    : _managedDataPath{std::move(musicRoot)}
  {
    _managedDataPath /= kManagedDataDirectoryName;
  }

  std::filesystem::path LibraryPaths::managedDataPath() const
  {
    return _managedDataPath;
  }

  std::filesystem::path LibraryPaths::databasePath() const
  {
    return _managedDataPath / kDatabaseDirectoryName;
  }

  std::filesystem::path LibraryPaths::logsPath() const
  {
    return _managedDataPath / kLogsDirectoryName;
  }

  bool LibraryPaths::hasExistingDatabase() const
  {
    return std::filesystem::exists(databasePath() / kLmdbDataFileName);
  }
} // namespace ao::rt
