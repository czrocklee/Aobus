// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>

#include <glibmm/refptr.h>
#include <glibmm/ustring.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace ao::audio
{
  struct TrackPlaybackDescriptor;
}

namespace ao::gtk
{
  class TrackRowObject;

  /**
   * TrackRowCache - Central repository and shared cache for UI TrackRows.
   *
   * Responsibilities:
   * - Batch-load all track metadata from LMDB into memory exactly once.
   * - Cache and share Glib::RefPtr<TrackRowObject> instances across all playlists/tabs.
   * - Efficiently resolve dictionary strings for UI display.
   * - Provider manages its own transactions internally.
   */
  class TrackRowCache final
  {
  public:
    explicit TrackRowCache(library::MusicLibrary& ml);

    /**
     * Get the shared TrackRowObject for a given ID.
     * @return TrackRowObject if it was loaded, nullptr otherwise.
     */
    Glib::RefPtr<TrackRowObject> getTrackRow(TrackId id) const;

    /**
     * Get the shared TrackRowObject for a given ID, reusing a caller-provided reader.
     */
    Glib::RefPtr<TrackRowObject> getTrackRow(TrackId id, library::TrackStore::Reader const& reader) const;

    /**
     * Resolve a dictionary string and cache it.
     */
    Glib::ustring const& resolveDictionaryString(DictionaryId id) const;

    /**
     * Get cover art resource ID for a track (direct from DB).
     */
    std::optional<std::uint32_t> getCoverArtId(TrackId id) const;

    /**
     * Get URI path for a track (direct from DB).
     */
    std::optional<std::filesystem::path> getUriPath(TrackId id) const;

    /**
     * Get playback descriptor for a track (direct from DB).
     */
    std::optional<audio::TrackPlaybackDescriptor> getPlaybackDescriptor(TrackId id) const;

    /**
     * Invalidate entry for a track (after updates).
     */
    void invalidate(TrackId id) const;

    /**
     * Remove track from cache (after deletion).
     */
    void remove(TrackId id);

    /**
     * Clear all cached rows and strings without reloading.
     * Subsequent getTrackRow() calls will lazily reload from the database.
     */
    void clearCache();

    /**
     * Get the dictionary store reference.
     */
    library::DictionaryStore const& dictionary() const { return _dict; }

  private:
    library::MusicLibrary& _ml;
    library::TrackStore& _store;
    library::DictionaryStore& _dict;

    mutable std::unordered_map<TrackId, Glib::RefPtr<TrackRowObject>> _rowCache;
    mutable std::unordered_map<DictionaryId, Glib::ustring> _stringCache;

    Glib::RefPtr<TrackRowObject> createRowFromView(TrackId id, library::TrackView const& view) const;
  };
} // namespace ao::gtk
