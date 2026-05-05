// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>

#include <deque>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace YAML
{
  class Node;
}

namespace ao::library
{
  class TrackBuilder;

  /**
   * Importer - Logical YAML importer for MusicLibrary.
   */
  class Importer final
  {
  public:
    explicit Importer(MusicLibrary& ml);

    /**
     * Import the library from a YAML file.
     * @param path Source file path.
     * @throws std::exception on failure.
     */
    void importFromYaml(std::filesystem::path const& path);

  private:
    void importTracks(YAML::Node const& tracks,
                      ao::lmdb::WriteTransaction& txn,
                      std::unordered_map<std::uint32_t, TrackId>& yamlTrackIdToInternalId);
    void importLists(YAML::Node const& lists,
                     ao::lmdb::WriteTransaction& txn,
                     std::unordered_map<std::uint32_t, TrackId> const& yamlTrackIdToInternalId);

    void overlayMetadata(TrackBuilder& builder,
                         YAML::Node const& trackNode,
                         std::deque<std::string>& trackStrings) const;
    void overlayCustomData(TrackBuilder& builder,
                           YAML::Node const& trackNode,
                           std::deque<std::string>& trackStrings) const;
    void overlayTechnicalProperties(TrackBuilder& builder, YAML::Node const& trackNode) const;

    MusicLibrary& _ml;
  };
} // namespace ao::library
