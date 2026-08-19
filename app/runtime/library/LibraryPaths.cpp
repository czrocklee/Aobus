// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/LibraryPaths.h>

#include <ao/utility/Path.h>

#include <filesystem>
#include <system_error>
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
    _managedDataPath /= utility::pathFromUtf8(kManagedDataDirectoryName);
  }

  std::filesystem::path LibraryPaths::managedDataPath() const
  {
    return _managedDataPath;
  }

  std::filesystem::path LibraryPaths::databasePath() const
  {
    return _managedDataPath / utility::pathFromUtf8(kDatabaseDirectoryName);
  }

  std::filesystem::path LibraryPaths::logsPath() const
  {
    return _managedDataPath / utility::pathFromUtf8(kLogsDirectoryName);
  }

  bool LibraryPaths::hasExistingDatabase() const
  {
    // Nonempty rather than merely present. The Windows data-file preparation
    // creates the file before LMDB initializes it, so a preparation or open that
    // fails afterwards leaves an empty one behind. Reading that as an existing
    // library would skip the first scan and leave the library permanently empty.
    // Anything with content is left to open to validate or reject.
    auto error = std::error_code{};
    auto const size = std::filesystem::file_size(databasePath() / utility::pathFromUtf8(kLmdbDataFileName), error);
    return !error && size > 0;
  }
} // namespace ao::rt
