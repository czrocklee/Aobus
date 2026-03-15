/*
 * Copyright (C) 2025 RockStudio
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <rs/core/TrackRecord.h>

namespace rs::core
{
  // Bloom filter uses 5 bits per tag (bit mask 31 = 0x1F)
  constexpr unsigned int kBloomBitMask = 31;

  TrackRecord::TrackRecord(TrackView const& view, Dictionary const& dict, lmdb::ReadTransaction& txn)
  {
    if (!view.isValid())
    {
      return;
    }

    // Copy property fields
    auto prop = view.property();
    property.fileSize = prop.fileSize();
    property.mtime = prop.mtime();
    property.durationMs = prop.durationMs();
    property.bitrate = prop.bitrate();
    property.sampleRate = prop.sampleRate();
    property.codecId = prop.codecId();
    property.channels = prop.channels();
    property.bitDepth = prop.bitDepth();
    property.rating = prop.rating();

    // Get metadata strings
    auto meta = view.metadata();
    metadata.title = std::string(meta.title());
    metadata.year = meta.year();
    metadata.trackNumber = meta.trackNumber();
    metadata.coverArtId = meta.coverArtId();

    // Resolve dictionary IDs to strings
    if (meta.artistId() > 0)
    {
      metadata.artist = std::string(dict.getString(txn, DictionaryId{meta.artistId()}));
    }
    if (meta.albumId() > 0)
    {
      metadata.album = std::string(dict.getString(txn, DictionaryId{meta.albumId()}));
    }
    if (meta.albumArtistId() > 0)
    {
      metadata.albumArtist = std::string(dict.getString(txn, DictionaryId{meta.albumArtistId()}));
    }
    if (meta.genreId() > 0)
    {
      metadata.genre = std::string(dict.getString(txn, DictionaryId{meta.genreId()}));
    }

    // Deserialize tag IDs from payload
    auto tagProxy = view.tags();
    auto tagCount = tagProxy.count();
    for (std::uint8_t i = 0; i < tagCount; ++i)
    {
      auto tagId = tagProxy.id(i);
      if (tagId > 0)
      {
        tags.ids.push_back(tagId);
      }
    }
  }

  TrackHeader TrackRecord::header() const
  {
    TrackHeader h{};
    h.fileSize = property.fileSize;
    h.mtime = property.mtime;
    h.durationMs = property.durationMs;
    h.bitrate = property.bitrate;
    h.sampleRate = property.sampleRate;
    h.year = metadata.year;
    h.trackNumber = metadata.trackNumber;
    h.coverArtId = metadata.coverArtId;
    h.channels = property.channels;
    h.bitDepth = property.bitDepth;
    h.rating = property.rating;
    h.codecId = property.codecId;
    h.tagCount = static_cast<std::uint8_t>(tags.ids.size());

    // Compute bloom filter from tag IDs
    std::uint32_t bloom = 0;
    for (auto tagId : tags.ids)
    {
      bloom |= (1U << (tagId.value() & kBloomBitMask));
    }
    h.tagBloom = bloom;

    return h;
  }

  std::vector<std::byte> TrackRecord::serialize() const
  {
    std::vector<std::byte> data;

    // Build header
    auto h = header();

    // Calculate offsets - tags first (hot data for filtering)
    std::uint16_t tagsOffset = 0;
    std::uint16_t titleOffset = static_cast<std::uint16_t>(tags.ids.size() * sizeof(std::uint32_t));
    std::uint16_t uriOffset = static_cast<std::uint16_t>(titleOffset + metadata.title.size() + 1);

    h.titleOffset = titleOffset;
    h.titleLen = static_cast<std::uint16_t>(metadata.title.size());
    h.uriOffset = uriOffset;
    h.uriLen = static_cast<std::uint16_t>(metadata.uri.size());
    h.tagsOffset = tagsOffset;

    // Write header
    auto* headerPtr = reinterpret_cast<std::byte const*>(&h);
    data.insert(data.end(), headerPtr, headerPtr + sizeof(TrackHeader));

    // Write tags first: 4-byte tag IDs
    for (auto tagId : tags.ids)
    {
      auto* idPtr = reinterpret_cast<std::byte const*>(&tagId);
      data.insert(data.end(), idPtr, idPtr + sizeof(std::uint32_t));
    }

    // Write title
    auto* titlePtr = reinterpret_cast<std::byte const*>(metadata.title.data());
    data.insert(data.end(), titlePtr, titlePtr + metadata.title.size());
    data.push_back(static_cast<std::byte>('\0'));

    // Write uri
    auto* uriPtr = reinterpret_cast<std::byte const*>(metadata.uri.data());
    data.insert(data.end(), uriPtr, uriPtr + metadata.uri.size());
    data.push_back(static_cast<std::byte>('\0'));

    return data;
  }

} // namespace rs::core
