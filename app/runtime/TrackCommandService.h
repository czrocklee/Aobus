// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/Type.h"

#include <filesystem>
#include <string>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class LibraryMutationService;

  /**
   * Frontend-neutral service for track-related business operations.
   * Safe to use from both CLI and GUI.
   */
  class TrackCommandService final
  {
  public:
    TrackCommandService(library::MusicLibrary& library, LibraryMutationService& mutation);
    ~TrackCommandService() = default;

    TrackCommandService(TrackCommandService const&) = delete;
    TrackCommandService& operator=(TrackCommandService const&) = delete;
    TrackCommandService(TrackCommandService&&) = delete;
    TrackCommandService& operator=(TrackCommandService&&) = delete;

    /**
     * Adds a tag to a track. Returns true if the tag was added.
     */
    bool addTag(TrackId trackId, std::string const& tagName);

    /**
     * Removes a tag from a track. Returns true if the tag was removed.
     */
    bool removeTag(TrackId trackId, std::string const& tagName);

    /**
     * Deletes a track from the library.
     */
    bool deleteTrack(TrackId trackId);

    /**
     * Creates a new track from a file path. Returns the new track ID.
     */
    TrackId createTrackFromFile(std::filesystem::path const& path);

  private:
    library::MusicLibrary& _library;
    LibraryMutationService& _mutation;
  };
} // namespace ao::rt
