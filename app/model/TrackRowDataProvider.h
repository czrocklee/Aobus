// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "TrackIdList.h"

#include <rs/core/DictionaryStore.h>
#include <rs/core/MusicLibrary.h>
#include <rs/core/TrackStore.h>

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace app::playback
{
  struct TrackPlaybackDescriptor;
}

namespace app::model
{

  /**
   * RowData - Owned DTO for row rendering.
   * All strings are owned (not string_view from DB-backed spans).
   */
  struct RowData
  {
    TrackId id;
    std::string artist;
    std::string album;
    std::string title;
    std::string tags;
    std::optional<std::uint32_t> coverArtId;
    bool missing = false;
  };

  /**
   * TrackRowDataProvider - Lazy row data loading with owned DTOs.
   *
   * Responsibilities:
   * - Load row data on demand from TrackStore
   * - Cache owned RowData DTOs (not TrackView)
   * - Cache resolved dictionary strings
   * - Provide cover art ID and URI path lookups
   * - Explicit invalidation by TrackId
   *
   * Design rules:
   * - Never cache TrackView, spans, or string_view from LMDB-backed buffers
   * - Cache only owned strings/numbers in DTOs
   * - Provider manages its own transactions internally
   */
  class TrackRowDataProvider final
  {
  public:
    explicit TrackRowDataProvider(rs::core::MusicLibrary& ml);

    /**
     * Get row data for a track. Loads lazily and caches result.
     * Creates internal transaction.
     * @return RowData if track exists, std::nullopt otherwise
     */
    std::optional<RowData> getRow(TrackId id);

    /**
     * Get cover art resource ID for a track.
     * Creates internal transaction.
     * @return coverArtId if track has art, std::nullopt otherwise
     */
    std::optional<std::uint32_t> getCoverArtId(TrackId id);

    /**
     * Get URI path for a track.
     * Creates internal transaction.
     * @return URI path if track exists, std::nullopt otherwise
     */
    std::optional<std::filesystem::path> getUriPath(TrackId id);

    /**
     * Get playback descriptor for a track.
     * Creates internal transaction.
     * @return TrackPlaybackDescriptor if track exists, std::nullopt otherwise
     */
    std::optional<app::playback::TrackPlaybackDescriptor> getPlaybackDescriptor(TrackId id);

    /**
     * Invalidate hot data cache for a track (after tag updates).
     */
    void invalidateHot(TrackId id);

    /**
     * Invalidate full cache for a track (after import or full update).
     */
    void invalidateFull(TrackId id);

    /**
     * Remove track from cache (after deletion).
     */
    void remove(TrackId id);

    /**
     * Get the dictionary store reference.
     */
    rs::core::DictionaryStore const& dictionary() const { return *_dict; }

  private:
    std::string resolveDictionaryString(rs::core::DictionaryId id);

    rs::core::MusicLibrary* _ml;
    rs::core::TrackStore* _store;
    rs::core::DictionaryStore* _dict;

    std::unordered_map<TrackId, RowData> _rowCache;
    std::unordered_map<rs::core::DictionaryId, std::string> _stringCache;
  };

} // namespace app::model