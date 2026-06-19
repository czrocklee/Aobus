// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "track/TrackRowCache.h"

#include "track/TrackRowObject.h"
#include <ao/Type.h>
#include <ao/audio/Types.h>
#include <ao/library/CoverArt.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/utility/Log.h>

#include <glibmm/refptr.h>
#include <glibmm/ustring.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    Glib::ustring joinResolvedTags(library::TrackView::TagProxy tags, library::DictionaryStore const& dictionary)
    {
      auto text = Glib::ustring{};
      bool first = true;

      for (auto const tagId : tags)
      {
        auto const tag = dictionary.getOrDefault(tagId, "");

        if (tag.empty())
        {
          if (tagId.raw() != 0)
          {
            APP_LOG_ERROR("TrackRowCache: invalid tag ID {} not found in dictionary", tagId.raw());
          }

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

  TrackRowCache::TrackRowCache(library::MusicLibrary& ml)
    : _ml{ml}, _store{ml.tracks()}, _dict{ml.dictionary()}
  {
  }

  Glib::RefPtr<TrackRowObject> TrackRowCache::createRowFromView(TrackId id,
                                                                library::TrackView const& view,
                                                                lmdb::ReadTransaction const& txn) const
  {
    auto const rowPtr = TrackRowObject::create(id, *this);

    auto const& metadata = view.metadata();
    auto const title = metadata.title();

    auto fileSize = std::uint64_t{0};
    auto mtime = std::uint64_t{0};
    auto status = library::FileStatus::Available;

    if (auto const uri = view.property().uri(); !uri.empty())
    {
      // Reuse the caller's read transaction for the manifest lookup rather than
      // opening a second one per row.
      auto const reader = _ml.manifest().reader(txn);

      if (auto const optManifestView = reader.get(uri); optManifestView)
      {
        fileSize = optManifestView->fileSize();
        mtime = optManifestView->mtime();
        status = optManifestView->status();
      }
    }

    rowPtr->populate(Glib::ustring{title.begin(), title.end()},
                     metadata.artistId(),
                     metadata.albumId(),
                     metadata.albumArtistId(),
                     metadata.genreId(),
                     metadata.composerId(),
                     metadata.workId(),
                     metadata.movementId(),
                     joinResolvedTags(view.tags(), _dict),
                     view.property().duration(),
                     metadata.year(),
                     metadata.discNumber(),
                     metadata.discTotal(),
                     metadata.trackNumber(),
                     metadata.trackTotal(),
                     metadata.movementNumber(),
                     metadata.movementTotal(),
                     view.coverArt()
                       .primary()
                       .transform([](library::CoverArt cover) { return cover.resourceId; })
                       .value_or(kInvalidResourceId),
                     view.property().sampleRate().raw(),
                     view.property().channels().raw(),
                     view.property().bitDepth().raw(),
                     view.property().codec(),
                     view.property().bitrate().raw(),
                     fileSize,
                     mtime,
                     status);

    return rowPtr;
  }

  Glib::RefPtr<TrackRowObject> TrackRowCache::trackRow(TrackId id) const
  {
    if (auto const it = _rowCache.find(id); it != _rowCache.end())
    {
      return it->second;
    }

    // Lazy load the row if it's missing from the cache (e.g., after an invalidate)
    auto const txn = _ml.readTransaction();
    auto const reader = _store.reader(txn);
    auto const optView = reader.get(id, library::TrackStore::Reader::LoadMode::Both);

    if (!optView)
    {
      return nullptr;
    }

    auto const rowPtr = createRowFromView(id, *optView, txn);
    _rowCache[id] = rowPtr;
    return rowPtr;
  }

  ResourceId TrackRowCache::coverArtId(TrackId id) const
  {
    auto const txn = _ml.readTransaction();
    auto const reader = _store.reader(txn);
    auto const optView = reader.get(id, library::TrackStore::Reader::LoadMode::Both);

    if (!optView)
    {
      return kInvalidResourceId;
    }

    auto const optPrimary = optView->coverArt().primary();

    if (!optPrimary)
    {
      return kInvalidResourceId;
    }

    return optPrimary->resourceId;
  }

  std::optional<std::filesystem::path> TrackRowCache::uriPath(TrackId id) const
  {
    // Need cold data for URI
    auto const txn = _ml.readTransaction();
    auto const reader = _store.reader(txn);

    auto const optView = reader.get(id, library::TrackStore::Reader::LoadMode::Both);

    if (!optView)
    {
      return std::nullopt;
    }

    return resolveLibraryPath(_ml.rootPath(), optView->property().uri());
  }

  std::optional<audio::TrackPlaybackDescriptor> TrackRowCache::playbackDescriptor(TrackId id) const
  {
    // Need cold data for URI and property info
    auto const txn = _ml.readTransaction();
    auto const reader = _store.reader(txn);

    auto const optView = reader.get(id, library::TrackStore::Reader::LoadMode::Both);

    if (!optView)
    {
      return std::nullopt;
    }

    auto const& view = *optView;
    auto const& metadata = view.metadata();
    auto const& property = view.property();

    auto desc = audio::TrackPlaybackDescriptor{.trackId = id,
                                               .duration = property.duration(),
                                               .sampleRateHint = property.sampleRate().raw(),
                                               .channelsHint = property.channels().raw(),
                                               .bitDepthHint = property.bitDepth().raw()};

    // File path
    if (auto const optFilePath = resolveLibraryPath(_ml.rootPath(), property.uri()); optFilePath)
    {
      desc.filePath = *optFilePath;
    }

    // Title
    desc.title = std::string{metadata.title()};

    // Artist
    if (auto const artistId = metadata.artistId(); artistId != kInvalidDictionaryId)
    {
      desc.artist = resolveDictionaryString(artistId).raw();
    }

    // Album
    if (auto const albumId = metadata.albumId(); albumId != kInvalidDictionaryId)
    {
      desc.album = resolveDictionaryString(albumId).raw();
    }

    // Cover art
    if (auto const optPrimary = view.coverArt().primary(); optPrimary)
    {
      desc.coverArtId = optPrimary->resourceId;
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
    auto const str = _dict.getOrDefault(id, "");
    auto result = Glib::ustring{str.begin(), str.end()};

    auto const insertResult = _stringCache.emplace(id, std::move(result));
    return insertResult.first->second;
  }
} // namespace ao::gtk
