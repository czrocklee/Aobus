// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "ao/Error.h"
#include "ao/library/MusicLibrary.h"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace ao::rt
{
  /**
   * ImportMode - Controls how the library is imported from YAML.
   */
  enum class ImportMode : std::uint8_t
  {
    Restore, // Destructive within payload scope: clears existing data before rebuild.
    Merge    // Additive/update import: preserves existing records outside payload scope.
  };

  /**
   * Importer - Logical YAML importer for library::MusicLibrary.
   */
  class LibraryImporter final
  {
  public:
    explicit LibraryImporter(library::MusicLibrary& ml);
    ~LibraryImporter();

    LibraryImporter(LibraryImporter const&) = delete;
    LibraryImporter& operator=(LibraryImporter const&) = delete;
    LibraryImporter(LibraryImporter&&) noexcept = default;
    LibraryImporter& operator=(LibraryImporter&&) noexcept = default;

    /**
     * Import the library from a YAML file.
     * @param path Source file path.
     * @param mode Import mode. Defaults to Restore.
     * @return Result of the operation.
     */
    Result<> importFromYaml(std::filesystem::path const& path, ImportMode mode = ImportMode::Restore);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace ao::rt
