// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "track/TrackRowCache.h"

#include "track/TrackRowObject.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackRow.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>

#include <glibmm/refptr.h>
#include <glibmm/ustring.h>

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    Glib::ustring toUString(std::string&& value)
    {
      return Glib::ustring{std::move(value)};
    }
  }

  TrackRowCache::TrackRowCache(rt::Library const& reads)
    : _reads{reads}
  {
  }

  Glib::RefPtr<TrackRowObject> TrackRowCache::createRowObject(rt::TrackRow row) const
  {
    auto const rowPtr = TrackRowObject::create(row.id, *this);

    rowPtr->populate(toUString(std::move(row.title)),
                     toUString(std::move(row.artist)),
                     toUString(std::move(row.album)),
                     toUString(std::move(row.albumArtist)),
                     toUString(std::move(row.genre)),
                     toUString(std::move(row.composer)),
                     toUString(std::move(row.conductor)),
                     toUString(std::move(row.ensemble)),
                     toUString(std::move(row.work)),
                     toUString(std::move(row.movement)),
                     toUString(std::move(row.soloist)),
                     toUString(std::move(row.tags)),
                     row.duration,
                     row.year,
                     row.discNumber,
                     row.discTotal,
                     row.trackNumber,
                     row.trackTotal,
                     row.movementNumber,
                     row.movementTotal,
                     row.coverArtId,
                     row.sampleRate,
                     row.channels,
                     row.bitDepth,
                     row.codec,
                     row.bitrate,
                     row.fileSize,
                     row.modifiedTime,
                     row.status);

    return rowPtr;
  }

  Glib::RefPtr<TrackRowObject> TrackRowCache::trackRow(TrackId id) const
  {
    if (auto const it = _rowCache.find(id); it != _rowCache.end())
    {
      return it->second;
    }

    auto scope = _reads.reader();
    auto optRow = scope.trackRow(id);

    if (!optRow)
    {
      return nullptr;
    }

    auto const rowPtr = createRowObject(std::move(*optRow));
    _rowCache.emplace(id, rowPtr);
    return rowPtr;
  }

  ResourceId TrackRowCache::coverArtId(TrackId id) const
  {
    auto scope = _reads.reader();
    return scope.trackCoverArtId(id);
  }

  std::optional<std::filesystem::path> TrackRowCache::uriPath(TrackId id) const
  {
    auto scope = _reads.reader();
    return scope.trackUriPath(id);
  }

  void TrackRowCache::invalidate(TrackId id) const
  {
    _rowCache.erase(id);
  }

  void TrackRowCache::remove(TrackId id)
  {
    _rowCache.erase(id);
  }

  void TrackRowCache::clearCache()
  {
    _rowCache.clear();
    _stringCache.clear();
  }

  Glib::ustring const& TrackRowCache::resolveDictionaryString(DictionaryId id) const
  {
    // Check cache first
    if (auto const it = _stringCache.find(id); it != _stringCache.end())
    {
      return it->second;
    }

    auto scope = _reads.reader();
    auto const str = scope.resolve(id);
    auto result = Glib::ustring{str.begin(), str.end()};

    auto const insertResult = _stringCache.emplace(id, std::move(result));
    return insertResult.first->second;
  }
} // namespace ao::gtk
