// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/library/MusicLibrary.h>

#include <filesystem>
#include <string>
#include <unordered_map>

namespace YAML
{
  class Node;
}

namespace rs::library
{

  /**
   * LibraryImporter - Logical YAML importer for MusicLibrary.
   */
  class LibraryImporter final
  {
  public:
    explicit LibraryImporter(MusicLibrary& ml);

    /**
     * Import the library from a YAML file.
     * @param path Source file path.
     * @throws std::exception on failure.
     */
    void importFromYaml(std::filesystem::path const& path);

  private:
    void importTracks(YAML::Node const& tracks,
                      rs::lmdb::WriteTransaction& txn,
                      std::unordered_map<std::uint32_t, TrackId>& yamlTrackIdToInternalId);
    void importLists(YAML::Node const& lists,
                     rs::lmdb::WriteTransaction& txn,
                     std::unordered_map<std::uint32_t, TrackId> const& yamlTrackIdToInternalId);

    MusicLibrary& _ml;
  };

} // namespace rs::library
