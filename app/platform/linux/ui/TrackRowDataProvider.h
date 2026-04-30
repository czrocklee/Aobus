// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/DictionaryStore.h>
#include <rs/core/MusicLibrary.h>
#include <rs/core/TrackStore.h>

#include <glibmm/refptr.h>
#include <glibmm/ustring.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace app::core::playback
{
  struct TrackPlaybackDescriptor;
}

namespace app::ui
{
  class TrackRow;

  /**
   * TrackRowDataProvider - Central repository and shared cache for UI TrackRows.
   *
   * Responsibilities:
   * - Batch-load all track metadata from LMDB into memory exactly once.
   * - Cache and share Glib::RefPtr<TrackRow> instances across all playlists/tabs.
   * - Efficiently resolve dictionary strings for UI display.
   * - Provider manages its own transactions internally.
   */
  class TrackRowDataProvider final
  {
  public:
    using TrackId = rs::core::TrackId;

    explicit TrackRowDataProvider(rs::core::MusicLibrary& ml);

    /**
     * Batch-load all tracks from the library into the central cache.
     * This avoids multiple LMDB transactions during UI sorting.
     */
    void loadAll();

    /**
     * Get the shared TrackRow for a given ID.
     * @return TrackRow if it was loaded, nullptr otherwise.
     */
    Glib::RefPtr<TrackRow> getTrackRow(TrackId id) const;

    /**
     * Resolve a dictionary string and cache it.
     */
    Glib::ustring const& resolveDictionaryString(rs::core::DictionaryId id) const;

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
    std::optional<app::core::playback::TrackPlaybackDescriptor> getPlaybackDescriptor(TrackId id) const;

    /**
     * Invalidate entry for a track (after updates).
     */
    void invalidate(TrackId id);

    /**
     * Remove track from cache (after deletion).
     */
    void remove(TrackId id);

    /**
     * Get the dictionary store reference.
     */
    rs::core::DictionaryStore const& dictionary() const { return _dict; }

  private:
    rs::core::MusicLibrary& _ml;
    rs::core::TrackStore& _store;
    rs::core::DictionaryStore& _dict;

    mutable std::unordered_map<TrackId, Glib::RefPtr<TrackRow>> _rowCache;
    mutable std::unordered_map<rs::core::DictionaryId, Glib::ustring> _stringCache;
  };

} // namespace app::ui
