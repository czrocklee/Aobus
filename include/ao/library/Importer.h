// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/library/MusicLibrary.h>

#include <filesystem>
#include <memory>

namespace ao::library
{
  /**
   * Importer - Logical YAML importer for MusicLibrary.
   */
  class Importer final
  {
  public:
    explicit Importer(MusicLibrary& ml);
    ~Importer();

    Importer(Importer const&) = delete;
    Importer& operator=(Importer const&) = delete;
    Importer(Importer&&) noexcept = default;
    Importer& operator=(Importer&&) noexcept = default;

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
} // namespace ao::library
