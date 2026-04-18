// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TrackRowDataProvider.h"

#include "playback/PlaybackTypes.h"

#include <string_view>

namespace
{
  auto joinResolvedTags(rs::core::TrackView::TagProxy tags, rs::core::DictionaryStore const& dictionary) -> std::string
  {
    auto text = std::string{};
    auto first = true;

    for (auto const tagId : tags)
    {
      auto const tag = dictionary.get(tagId);
      if (tag.empty())
      {
        continue;
      }

      if (!first)
      {
        text += ", ";
      }

      text += tag;
      first = false;
    }

    return text;
  }
}

namespace
{
  auto resolveLibraryPath(std::filesystem::path const& libraryRoot, std::string_view uri)
    -> std::optional<std::filesystem::path>
  {
    if (uri.empty())
    {
      return std::nullopt;
    }

    auto path = std::filesystem::path{uri};
    if (path.is_absolute())
    {
      return path.lexically_normal();
    }

    return (libraryRoot / path).lexically_normal();
  }
}

namespace app::model
{

  TrackRowDataProvider::TrackRowDataProvider(rs::core::MusicLibrary& ml)
    : _ml{&ml}, _store{&ml.tracks()}, _dict{&ml.dictionary()}
  {
  }

  std::optional<RowData> TrackRowDataProvider::getRow(TrackId id)
  {
    // Check cache first
    auto const it = _rowCache.find(id);
    if (it != _rowCache.end())
    {
      if (it->second.missing)
      {
        return std::nullopt;
      }

      return it->second;
    }

    // Load both tiers so grouping and section sorting can use stable row-local keys.
    rs::lmdb::ReadTransaction txn(_ml->readTransaction());
    auto reader = _store->reader(txn);

    auto const optView = reader.get(id, rs::core::TrackStore::Reader::LoadMode::Both);
    if (!optView)
    {
      // Track not found - cache a "missing" marker
      RowData row;
      row.id = id;
      row.missing = true;
      _rowCache.emplace(id, std::move(row));
      return std::nullopt;
    }

    auto const& view = *optView;
    auto const& metadata = view.metadata();

    RowData row;
    row.id = id;
    row.title = std::string(metadata.title());

    // Resolve dictionary strings (artist, album) and cache them
    auto const artistId = metadata.artistId();
    if (artistId != rs::core::DictionaryId{0})
    {
      row.artist = resolveDictionaryString(artistId);
    }

    auto const albumId = metadata.albumId();
    if (albumId != rs::core::DictionaryId{0})
    {
      row.album = resolveDictionaryString(albumId);
    }

    auto const albumArtistId = metadata.albumArtistId();
    if (albumArtistId != rs::core::DictionaryId{0})
    {
      row.albumArtist = resolveDictionaryString(albumArtistId);
    }

    auto const genreId = metadata.genreId();
    if (genreId != rs::core::DictionaryId{0})
    {
      row.genre = resolveDictionaryString(genreId);
    }

    row.year = metadata.year();
    row.discNumber = metadata.discNumber();
    row.trackNumber = metadata.trackNumber();

    auto const coverArtId = metadata.coverArtId();
    if (coverArtId != 0)
    {
      row.coverArtId = coverArtId;
    }

    row.tags = joinResolvedTags(view.tags(), *_dict);

    auto const result = _rowCache.emplace(id, std::move(row));
    return result.first->second;
  }

  std::optional<std::uint32_t> TrackRowDataProvider::getCoverArtId(TrackId id)
  {
    // Need cold data for coverArtId
    rs::lmdb::ReadTransaction txn(_ml->readTransaction());
    auto reader = _store->reader(txn);

    auto const optView = reader.get(id, rs::core::TrackStore::Reader::LoadMode::Both);
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

  std::optional<std::filesystem::path> TrackRowDataProvider::getUriPath(TrackId id)
  {
    // Need cold data for URI
    rs::lmdb::ReadTransaction txn(_ml->readTransaction());
    auto reader = _store->reader(txn);

    auto const optView = reader.get(id, rs::core::TrackStore::Reader::LoadMode::Both);
    if (!optView)
    {
      return std::nullopt;
    }

    return resolveLibraryPath(_ml->rootPath(), optView->property().uri());
  }

  std::optional<app::playback::TrackPlaybackDescriptor> TrackRowDataProvider::getPlaybackDescriptor(TrackId id)
  {
    // Need cold data for URI and property info
    rs::lmdb::ReadTransaction txn(_ml->readTransaction());
    auto reader = _store->reader(txn);

    auto const optView = reader.get(id, rs::core::TrackStore::Reader::LoadMode::Both);
    if (!optView)
    {
      return std::nullopt;
    }

    auto const& view = *optView;
    auto const& metadata = view.metadata();
    auto const& property = view.property();

    app::playback::TrackPlaybackDescriptor desc;
    desc.trackId = id;

    // File path
    if (auto const filePath = resolveLibraryPath(_ml->rootPath(), property.uri()))
    {
      desc.filePath = *filePath;
    }

    // Title
    desc.title = std::string(metadata.title());

    // Artist
    auto const artistId = metadata.artistId();
    if (artistId != rs::core::DictionaryId{0})
    {
      desc.artist = resolveDictionaryString(artistId);
    }

    // Album
    auto const albumId = metadata.albumId();
    if (albumId != rs::core::DictionaryId{0})
    {
      desc.album = resolveDictionaryString(albumId);
    }

    // Cover art
    auto const coverArtId = metadata.coverArtId();
    if (coverArtId != 0)
    {
      desc.coverArtId = rs::core::ResourceId{coverArtId};
    }

    // Duration
    desc.durationMs = property.durationMs();

    // Technical properties (hints for decoder)
    desc.sampleRateHint = property.sampleRate();
    desc.channelsHint = property.channels();
    desc.bitDepthHint = property.bitDepth();

    return desc;
  }

  void TrackRowDataProvider::invalidateHot(TrackId id)
  {
    // For hot invalidation, just remove from cache - will reload on next getRow
    _rowCache.erase(id);
  }

  void TrackRowDataProvider::invalidateFull(TrackId id)
  {
    _rowCache.erase(id);
  }

  void TrackRowDataProvider::remove(TrackId id)
  {
    _rowCache.erase(id);
  }

  std::string TrackRowDataProvider::resolveDictionaryString(rs::core::DictionaryId id)
  {
    // Check cache first
    auto const it = _stringCache.find(id);
    if (it != _stringCache.end())
    {
      return it->second;
    }

    // Resolve from dictionary and cache
    // DictionaryStore::get throws if not found, so we catch it
    std::string result;
    try
    {
      auto const str = _dict->get(id);
      result = std::string(str);
    }
    catch (std::exception const&)
    {
      // Dictionary ID not found - return empty string
      result = {};
    }

    auto const insertResult = _stringCache.emplace(id, result);
    return insertResult.first->second;
  }

} // namespace app::model
