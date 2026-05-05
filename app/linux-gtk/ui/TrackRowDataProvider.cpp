// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TrackRowDataProvider.h"

#include "TrackRow.h"
#include <ao/audio/Types.h>

#include <string_view>

namespace
{
  Glib::ustring joinResolvedTags(ao::library::TrackView::TagProxy tags, ao::library::DictionaryStore const& dictionary)
  {
    auto text = Glib::ustring{};
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

    auto path = std::filesystem::path{uri};

    if (path.is_absolute())
    {
      return path.lexically_normal();
    }

    return (libraryRoot / path).lexically_normal();
  }
}

namespace ao::gtk
{
  TrackRowDataProvider::TrackRowDataProvider(ao::library::MusicLibrary& ml)
    : _ml{ml}, _store{ml.tracks()}, _dict{ml.dictionary()}
  {
  }

  void TrackRowDataProvider::loadAll()
  {
    _rowCache.clear();
    _stringCache.clear();

    ao::lmdb::ReadTransaction txn(_ml.readTransaction());
    auto reader = _store.reader(txn);

    for (auto const& [id, view] : reader)
    {
      _rowCache[id] = createRowFromView(id, view);
    }
  }

  Glib::RefPtr<TrackRow> TrackRowDataProvider::createRowFromView(TrackId id, ao::library::TrackView const& view) const
  {
    auto row = TrackRow::create(id, *this);

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

  Glib::RefPtr<TrackRow> TrackRowDataProvider::getTrackRow(TrackId id) const
  {
    auto const it = _rowCache.find(id);
    if (it != _rowCache.end())
    {
      return it->second;
    }

    // Lazy load the row if it's missing from the cache (e.g., after an invalidate)
    ao::lmdb::ReadTransaction txn(_ml.readTransaction());
    auto reader = _store.reader(txn);
    auto const optView = reader.get(id, ao::library::TrackStore::Reader::LoadMode::Both);

    if (!optView)
    {
      return nullptr;
    }

    auto row = createRowFromView(id, *optView);
    _rowCache[id] = row;
    return row;
  }

  std::optional<std::uint32_t> TrackRowDataProvider::getCoverArtId(TrackId id) const
  {
    // Need cold data for coverArtId
    ao::lmdb::ReadTransaction txn(_ml.readTransaction());
    auto reader = _store.reader(txn);
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

  std::optional<std::filesystem::path> TrackRowDataProvider::getUriPath(TrackId id) const
  {
    // Need cold data for URI
    ao::lmdb::ReadTransaction txn(_ml.readTransaction());
    auto reader = _store.reader(txn);

    auto const optView = reader.get(id, ao::library::TrackStore::Reader::LoadMode::Both);

    if (!optView)
    {
      return std::nullopt;
    }

    return resolveLibraryPath(_ml.rootPath(), optView->property().uri());
  }

  std::optional<ao::audio::TrackPlaybackDescriptor> TrackRowDataProvider::getPlaybackDescriptor(TrackId id) const
  {
    // Need cold data for URI and property info
    ao::lmdb::ReadTransaction txn(_ml.readTransaction());
    auto reader = _store.reader(txn);

    auto const optView = reader.get(id, ao::library::TrackStore::Reader::LoadMode::Both);

    if (!optView)
    {
      return std::nullopt;
    }

    auto const& view = *optView;
    auto const& metadata = view.metadata();
    auto const& property = view.property();

    ao::audio::TrackPlaybackDescriptor desc;
    desc.trackId = id;

    // File path
    if (auto const filePath = resolveLibraryPath(_ml.rootPath(), property.uri()))
    {
      desc.filePath = *filePath;
    }

    // Title
    desc.title = std::string{metadata.title()};

    // Artist
    if (auto const artistId = metadata.artistId(); artistId != ao::DictionaryId{0})
    {
      desc.artist = resolveDictionaryString(artistId).raw();
    }

    // Album
    if (auto const albumId = metadata.albumId(); albumId != ao::DictionaryId{0})
    {
      desc.album = resolveDictionaryString(albumId).raw();
    }

    // Cover art
    if (auto const coverArtId = metadata.coverArtId(); coverArtId != 0)
    {
      desc.optCoverArtId = ao::ResourceId{coverArtId};
    }

    // Duration
    desc.durationMs = property.durationMs();

    // Technical properties (hints for decoder)
    desc.sampleRateHint = property.sampleRate();
    desc.channelsHint = property.channels();
    desc.bitDepthHint = property.bitDepth();

    return desc;
  }

  void TrackRowDataProvider::invalidate(TrackId id)
  {
    _rowCache.erase(id);
  }

  void TrackRowDataProvider::remove(TrackId id)
  {
    _rowCache.erase(id);
  }

  Glib::ustring const& TrackRowDataProvider::resolveDictionaryString(ao::DictionaryId id) const
  {
    // Check cache first
    if (auto const it = _stringCache.find(id); it != _stringCache.end())
    {
      return it->second;
    }

    // Resolve from dictionary and cache
    Glib::ustring result;

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
