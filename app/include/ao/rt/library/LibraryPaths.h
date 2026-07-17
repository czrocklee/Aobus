// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <filesystem>

namespace ao::rt
{
  /**
   * Derives the canonical Aobus-managed paths for one music-library root.
   */
  class LibraryPaths final
  {
  public:
    explicit LibraryPaths(std::filesystem::path musicRoot);

    std::filesystem::path managedDataPath() const;
    std::filesystem::path databasePath() const;
    std::filesystem::path logsPath() const;
    bool hasExistingDatabase() const;

  private:
    std::filesystem::path _managedDataPath;
  };
} // namespace ao::rt
