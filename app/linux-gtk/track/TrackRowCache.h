// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "track/TrackRowPathDemand.h"
#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/i18n/MessageCatalog.h>

#include <boost/unordered/unordered_flat_map.hpp>
#include <glibmm/refptr.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace ao::rt
{
  class Library;
  class TrackListProjection;
  struct TrackListProjectionDeltaBatch;
  struct TrackRowRange;
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
   * A view with FilePath visible can include the path in its first row load.
   * A FilePath cell can also promote an already loaded path-free row at bind;
   * display text access never reads storage or the filesystem.
   *
   * GTK list-item identity is separate from this cache. Before publishing a
   * projection delta, the model refreshes affected rows with field subscribers.
   * Other holders must call trackRow() to refresh a stale field object. The ordinary
   * library observer handles changes even when no projection reports them.
   */
  class TrackRowCache final
  {
  public:
    TrackRowCache(rt::Library const& reads, i18n::MessageCatalog textCatalog);

    TrackRowCache(TrackRowCache const&) = delete;
    TrackRowCache& operator=(TrackRowCache const&) = delete;
    TrackRowCache(TrackRowCache&&) = delete;
    TrackRowCache& operator=(TrackRowCache&&) = delete;
    ~TrackRowCache() = default;

    /// Load on first access or refresh a stale row in place. Null means the track is absent.
    Glib::RefPtr<TrackRowObject> trackRow(TrackId id,
                                          TrackRowPathDemand pathDemand = TrackRowPathDemand::NoRequest) const;

    std::size_t cachedRowCount() const noexcept { return _rowCache.size(); }
    /// Number of snapshot row lookups, including in-place refreshes and misses.
    std::size_t rowLoadCount() const noexcept { return _rowLoadCount; }

    void prepareNotification(rt::TrackListProjection const& projection,
                             rt::TrackListProjectionDeltaBatch const& batch) const;

  private:
    rt::Library const& _reads;
    i18n::MessageCatalog _textCatalog;

    struct Entry final
    {
      Glib::RefPtr<TrackRowObject> rowPtr;
      std::uint64_t loadedRevision = 0;
      bool stale = false;
      // An unavailable path is still a completed attempt for this loaded row.
      bool pathAttempted = false;
    };

    mutable boost::unordered_flat_map<TrackId, Entry, std::hash<TrackId>> _rowCache;
    mutable std::size_t _rowLoadCount = 0;
    std::uint64_t _observedRevision = 0;
    // Declared last so it is released before the map its callback writes into.
    async::Subscription _changesSub;

    void markCachedRow(TrackId id, std::uint64_t revision, std::vector<TrackId>& bound) const;
    void markCachedRange(rt::TrackListProjection const& projection,
                         rt::TrackRowRange range,
                         std::uint64_t revision,
                         std::vector<TrackId>& bound) const;
    static void markStale(Entry& entry, std::uint64_t revision, std::vector<TrackId>& bound);
  };
} // namespace ao::gtk
