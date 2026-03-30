// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TrackRowDataProvider.h"

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
    if (it != _rowCache.end()) { return it->second; }

    // Load from store using Hot mode (sufficient for row rendering)
    rs::lmdb::ReadTransaction txn(_ml->readTransaction());
    auto reader = _store->reader(txn);

    auto const optView = reader.get(id, rs::core::TrackStore::Reader::LoadMode::Hot);
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
    if (artistId != rs::core::DictionaryId{0}) { row.artist = resolveDictionaryString(artistId); }

    auto const albumId = metadata.albumId();
    if (albumId != rs::core::DictionaryId{0}) { row.album = resolveDictionaryString(albumId); }

    // Tags - would need to resolve tag IDs to strings (placeholder for now)
    // TODO: Implement tag ID to string resolution if needed for row display

    // Cold data may not be available in Hot-only load, but coverArtId is in cold header
    // For row display, we don't need cover art - it's on the detail side

    auto const result = _rowCache.emplace(id, std::move(row));
    return result.first->second;
  }

  std::optional<std::uint32_t> TrackRowDataProvider::getCoverArtId(TrackId id)
  {
    // Need cold data for coverArtId
    rs::lmdb::ReadTransaction txn(_ml->readTransaction());
    auto reader = _store->reader(txn);

    auto const optView = reader.get(id, rs::core::TrackStore::Reader::LoadMode::Both);
    if (!optView) { return std::nullopt; }

    auto const coverArtId = optView->metadata().coverArtId();
    if (coverArtId == 0) { return std::nullopt; }
    return coverArtId;
  }

  std::optional<std::filesystem::path> TrackRowDataProvider::getUriPath(TrackId id)
  {
    // Need cold data for URI
    rs::lmdb::ReadTransaction txn(_ml->readTransaction());
    auto reader = _store->reader(txn);

    auto const optView = reader.get(id, rs::core::TrackStore::Reader::LoadMode::Both);
    if (!optView) { return std::nullopt; }

    auto const uri = optView->property().uri();
    if (uri.empty()) { return std::nullopt; }

    return std::filesystem::path{uri};
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
    if (it != _stringCache.end()) { return it->second; }

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