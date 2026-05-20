// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "ao/library/MusicLibrary.h"

#include <filesystem>
#include <memory>

namespace ao::rt
{
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
     * @throws std::exception on failure.
     */
    void importFromYaml(std::filesystem::path const& path);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace ao::rt
