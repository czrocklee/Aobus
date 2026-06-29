// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <boost/unordered/unordered_flat_map.hpp>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <unordered_map>

namespace ao::rt
{
  class Library;
  struct TrackRow;
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
    explicit TrackRowCache(rt::Library const& reads);

    /**
     * Get the shared TrackRowObject for a given ID.
     * @return TrackRowObject if it was loaded, nullptr otherwise.
     */
    Glib::RefPtr<TrackRowObject> trackRow(TrackId id) const;

    /**
     * Resolve a dictionary string and cache it.
     */
    Glib::ustring const& resolveDictionaryString(DictionaryId id) const;

    /**
     * Get cover art resource ID for a track (direct from DB).
     */
    ResourceId coverArtId(TrackId id) const;

    /**
     * Get URI path for a track (direct from DB).
     */
    std::optional<std::filesystem::path> uriPath(TrackId id) const;

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
     * Subsequent trackRow() calls will lazily reload from the database.
     */
    void clearCache();

  private:
    rt::Library const& _reads;

    mutable boost::unordered_flat_map<TrackId, Glib::RefPtr<TrackRowObject>, std::hash<TrackId>> _rowCache;
    mutable std::unordered_map<DictionaryId, Glib::ustring> _stringCache;

    Glib::RefPtr<TrackRowObject> createRowFromData(rt::TrackRow data) const;
  };
} // namespace ao::gtk
