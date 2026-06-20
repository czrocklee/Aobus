// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "track/TrackRowCache.h"

#include "track/TrackRowObject.h"
#include <ao/Type.h>
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
    Glib::ustring toUString(std::string const& value)
    {
      return Glib::ustring{value.begin(), value.end()};
    }
  }

  TrackRowCache::TrackRowCache(rt::Library const& reads)
    : _reads{reads}
  {
  }

  Glib::RefPtr<TrackRowObject> TrackRowCache::createRowFromData(rt::TrackRow const& data) const
  {
    auto const rowPtr = TrackRowObject::create(data.id, *this);

    rowPtr->populate(toUString(data.title),
                     toUString(data.artist),
                     toUString(data.album),
                     toUString(data.albumArtist),
                     toUString(data.genre),
                     toUString(data.composer),
                     toUString(data.work),
                     toUString(data.movement),
                     toUString(data.tags),
                     data.duration,
                     data.year,
                     data.discNumber,
                     data.discTotal,
                     data.trackNumber,
                     data.trackTotal,
                     data.movementNumber,
                     data.movementTotal,
                     data.coverArtId,
                     data.sampleRate,
                     data.channels,
                     data.bitDepth,
                     data.codec,
                     data.bitrate,
                     data.fileSize,
                     data.modifiedTime,
                     data.status);

    return rowPtr;
  }

  Glib::RefPtr<TrackRowObject> TrackRowCache::trackRow(TrackId id) const
  {
    if (auto const it = _rowCache.find(id); it != _rowCache.end())
    {
      return it->second;
    }

    auto scope = _reads.reader();
    auto const optData = scope.trackRow(id);

    if (!optData)
    {
      return nullptr;
    }

    auto const rowPtr = createRowFromData(*optData);
    _rowCache[id] = rowPtr;
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
