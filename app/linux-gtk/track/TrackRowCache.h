// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>

#include <boost/unordered/unordered_flat_map.hpp>
#include <glibmm/refptr.h>

#include <cstddef>
#include <functional>

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
   * - Load track metadata from LMDB on first request.
   * - Cache and share Glib::RefPtr<TrackRowObject> instances across all playlists/tabs.
   * - Provider manages its own transactions internally.
   *
   * Rows are fully resolved at load time: display formatting never re-enters
   * storage.
   */
  class TrackRowCache final
  {
  public:
    TrackRowCache(rt::Library const& reads, i18n::MessageCatalog textCatalog);

    /**
     * Get the shared TrackRowObject for a given ID.
     * @return TrackRowObject if it was loaded, nullptr otherwise.
     */
    Glib::RefPtr<TrackRowObject> trackRow(TrackId id) const;

    std::size_t cachedRowCount() const noexcept { return _rowCache.size(); }

    /**
     * Invalidate entry for a track (after updates or deletion).
     */
    void invalidate(TrackId id) const;

    /**
     * Clear all cached rows without reloading.
     * Subsequent trackRow() calls will lazily reload from the database.
     */
    void clearCache();

  private:
    rt::Library const& _reads;
    i18n::MessageCatalog _textCatalog;

    mutable boost::unordered_flat_map<TrackId, Glib::RefPtr<TrackRowObject>, std::hash<TrackId>> _rowCache;

    Glib::RefPtr<TrackRowObject> createRowObject(rt::TrackRow row) const;
  };
} // namespace ao::gtk
