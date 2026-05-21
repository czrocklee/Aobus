// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

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
    Restore, // Recreate tracks exactly from YAML, do not read physical file tags.
    Overlay  // Match tracks by URI and update metadata from YAML. (Future)
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
     * @throws std::exception on failure.
     */
    void importFromYaml(std::filesystem::path const& path, ImportMode mode = ImportMode::Restore);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace ao::rt
