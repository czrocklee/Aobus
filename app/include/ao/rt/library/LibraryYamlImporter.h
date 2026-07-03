// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace ao::library
{
  class MusicLibrary;
}

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

  struct ImportReport final
  {
    std::uint64_t tracksCreated = 0;
    std::uint64_t tracksUpdated = 0;
    std::uint64_t tracksDeleted = 0;
    std::uint64_t listsCreated = 0;
    std::uint64_t listsDeleted = 0;

    bool operator==(ImportReport const&) const = default;
  };

  /**
   * LibraryYamlImporter - Logical YAML importer for library::MusicLibrary.
   */
  class LibraryYamlImporter final
  {
  public:
    explicit LibraryYamlImporter(library::MusicLibrary& ml);
    ~LibraryYamlImporter();

    LibraryYamlImporter(LibraryYamlImporter const&) = delete;
    LibraryYamlImporter& operator=(LibraryYamlImporter const&) = delete;
    LibraryYamlImporter(LibraryYamlImporter&&) noexcept = default;
    LibraryYamlImporter& operator=(LibraryYamlImporter&&) noexcept = default;

    /**
     * Import the library from a YAML file.
     * @param path Source file path.
     * @param mode Import mode. Defaults to Restore.
     * @return Result of the operation.
     */
    Result<ImportReport> importFromYaml(std::filesystem::path const& path, ImportMode mode = ImportMode::Restore);
    // Runs the same import path, but returns before commit and skips metadata
    // header updates.
    Result<ImportReport> previewImportFromYaml(std::filesystem::path const& path,
                                               ImportMode mode = ImportMode::Restore);

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
