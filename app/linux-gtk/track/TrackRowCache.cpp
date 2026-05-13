// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "track/TrackRowCache.h"

#include "track/TrackRowObject.h"
#include <ao/audio/Types.h>

#include <string_view>

namespace
{
  Glib::ustring joinResolvedTags(ao::library::TrackView::TagProxy tags, ao::library::DictionaryStore const& dictionary)
  {
    auto text = Glib::ustring{};
    bool first = true;

    for (auto const tagId : tags)
    {
      auto const tag = dictionary.get(tagId);

      if (tag.empty())
      {
        continue;
      }

      if (!first)
      {
        text.append(", ");
      }

      text.append(tag.data(), tag.size());
      first = false;
    }

    return text;
  }

  std::optional<std::filesystem::path> resolveLibraryPath(std::filesystem::path const& libraryRoot,
                                                          std::string_view uri)
  {
    if (uri.empty())
    {
      return std::nullopt;
    }

    auto const path = std::filesystem::path{uri};

    if (path.is_absolute())
    {
      return path.lexically_normal();
    }

    return (libraryRoot / path).lexically_normal();
  }
}

namespace ao::gtk
{
  TrackRowCache::TrackRowCache(ao::library::MusicLibrary& ml)
    : _ml{ml}, _store{ml.tracks()}, _dict{ml.dictionary()}
  {
  }

  Glib::RefPtr<TrackRowObject> TrackRowCache::createRowFromView(TrackId id, ao::library::TrackView const& view) const
  {
    auto const row = TrackRowObject::create(id, *this);

    auto const& metadata = view.metadata();
    auto const title = metadata.title();

    row->populate(Glib::ustring(title.begin(), title.end()),
                  metadata.artistId(),
                  metadata.albumId(),
                  metadata.albumArtistId(),
                  metadata.genreId(),
                  metadata.composerId(),
                  metadata.workId(),
                  joinResolvedTags(view.tags(), _dict),
                  std::chrono::milliseconds{view.property().durationMs()},
                  metadata.year(),
                  metadata.discNumber(),
                  metadata.totalDiscs(),
                  metadata.trackNumber(),
                  metadata.coverArtId() != 0 ? std::optional<std::uint64_t>{metadata.coverArtId()} : std::nullopt,
                  view.property().sampleRate(),
                  view.property().channels(),
                  view.property().bitDepth(),
                  view.property().codecId());

    return row;
  }

  Glib::RefPtr<TrackRowObject> TrackRowCache::getTrackRow(TrackId id) const
  {
    auto const it = _rowCache.find(id);

    if (it != _rowCache.end())
    {
      return it->second;
    }

    // Lazy load the row if it's missing from the cache (e.g., after an invalidate)
    auto const txn = _ml.readTransaction();
    auto const reader = _store.reader(txn);
    auto const optView = reader.get(id, ao::library::TrackStore::Reader::LoadMode::Both);

    if (!optView)
    {
      return nullptr;
    }

    auto const row = createRowFromView(id, *optView);
    _rowCache[id] = row;
    return row;
  }

  Glib::RefPtr<TrackRowObject> TrackRowCache::getTrackRow(TrackId id,
                                                          ao::library::TrackStore::Reader const& reader) const
  {
    auto const it = _rowCache.find(id);

    if (it != _rowCache.end())
    {
      return it->second;
    }

    auto const optView = reader.get(id, ao::library::TrackStore::Reader::LoadMode::Both);

    if (!optView)
    {
      return nullptr;
    }

    auto const row = createRowFromView(id, *optView);
    _rowCache[id] = row;
    return row;
  }

  std::optional<std::uint32_t> TrackRowCache::getCoverArtId(TrackId id) const
  {
    // Need cold data for coverArtId
    auto const txn = _ml.readTransaction();
    auto const reader = _store.reader(txn);
    auto const optView = reader.get(id, ao::library::TrackStore::Reader::LoadMode::Both);

    if (!optView)
    {
      return std::nullopt;
    }

    auto const coverArtId = optView->metadata().coverArtId();

    if (coverArtId == 0)
    {
      return std::nullopt;
    }

    return coverArtId;
  }

  std::optional<std::filesystem::path> TrackRowCache::getUriPath(TrackId id) const
  {
    // Need cold data for URI
    auto const txn = _ml.readTransaction();
    auto const reader = _store.reader(txn);

    auto const optView = reader.get(id, ao::library::TrackStore::Reader::LoadMode::Both);

    if (!optView)
    {
      return std::nullopt;
    }

    return resolveLibraryPath(_ml.rootPath(), optView->property().uri());
  }

  std::optional<ao::audio::TrackPlaybackDescriptor> TrackRowCache::getPlaybackDescriptor(TrackId id) const
  {
    // Need cold data for URI and property info
    auto const txn = _ml.readTransaction();
    auto const reader = _store.reader(txn);

    auto const optView = reader.get(id, ao::library::TrackStore::Reader::LoadMode::Both);

    if (!optView)
    {
      return std::nullopt;
    }

    auto const& view = *optView;
    auto const& metadata = view.metadata();
    auto const& property = view.property();

    auto desc = ao::audio::TrackPlaybackDescriptor{.trackId = id,
                                                   .durationMs = property.durationMs(),
                                                   .sampleRateHint = property.sampleRate(),
                                                   .channelsHint = property.channels(),
                                                   .bitDepthHint = property.bitDepth()};

    // File path
    if (auto const optFilePath = resolveLibraryPath(_ml.rootPath(), property.uri()))
    {
      desc.filePath = *optFilePath;
    }

    // Title
    desc.title = std::string{metadata.title()};

    // Artist
    if (auto const artistId = metadata.artistId(); artistId != DictionaryId{0})
    {
      desc.artist = resolveDictionaryString(artistId).raw();
    }

    // Album
    if (auto const albumId = metadata.albumId(); albumId != DictionaryId{0})
    {
      desc.album = resolveDictionaryString(albumId).raw();
    }

    // Cover art
    if (auto const coverArtId = metadata.coverArtId(); coverArtId != 0)
    {
      desc.optCoverArtId = ResourceId{coverArtId};
    }

    return desc;
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

    // Resolve from dictionary and cache
    auto result = Glib::ustring{};

    try
    {
      auto const str = _dict.get(id);
      result = Glib::ustring(str.begin(), str.end());
    }
    catch (std::exception const&)
    {
      result.clear();
    }

    auto const insertResult = _stringCache.emplace(id, std::move(result));
    return insertResult.first->second;
  }
} // namespace ao::gtk
