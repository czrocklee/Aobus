// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/TrackRecord.h>
#include <rs/core/TrackLayout.h>
#include <rs/utility/ByteView.h>

namespace rs::core
{
  // Bloom filter uses 5 bits per tag (bit mask 31 = 0x1F)
  constexpr std::uint32_t kBloomBitMask = 31;

  TrackRecord::TrackRecord(TrackHotView const& hotView, TrackColdView const& coldView, DictionaryStore const& dict)
  {
    if (!hotView.isValid()) { return; }

    // Copy hot property fields
    auto prop = hotView.property();
    property.codecId = prop.codecId();
    property.bitDepth = prop.bitDepth();
    property.rating = prop.rating();

    // Copy cold property fields
    auto coldProp = coldView.property();
    property.fileSize = coldProp.fileSize();
    property.mtime = coldProp.mtime();
    property.durationMs = coldProp.durationMs();
    property.sampleRate = coldProp.sampleRate();
    property.bitrate = coldProp.bitrate();
    property.channels = coldProp.channels();

    // Get hot metadata strings
    auto meta = hotView.metadata();
    metadata.title = std::string{meta.title()};
    metadata.year = meta.year();

    // Resolve dictionary IDs to strings
    if (meta.artistId() > 0) { metadata.artist = std::string{dict.get(meta.artistId())}; }
    if (meta.albumId() > 0) { metadata.album = std::string{dict.get(meta.albumId())}; }
    if (meta.albumArtistId() > 0) { metadata.albumArtist = std::string{dict.get(meta.albumArtistId())}; }
    if (meta.genreId() > 0) { metadata.genre = std::string{dict.get(meta.genreId())}; }

    // Deserialize tag IDs from hot payload
    auto tagProxy = hotView.tags();
    auto tagCount = tagProxy.count();
    for (std::uint8_t i = 0; i < tagCount; ++i)
    {
      auto tagId = tagProxy.id(i);
      tags.ids.push_back(tagId);
    }

    // Copy cold fields
    cold.uri = std::string{coldView.uri()};
    cold.fileSize = coldView.fileSize();
    cold.mtime = coldView.mtime();
    cold.coverArtId = coldView.coverArtId();
    cold.trackNumber = coldView.trackNumber();
    cold.totalTracks = coldView.totalTracks();
    cold.discNumber = coldView.discNumber();
    cold.totalDiscs = coldView.totalDiscs();

    // Load custom meta from cold view
    customMeta = coldView.customMeta();
  }

  TrackHotHeader TrackRecord::hotHeader() const
  {
    // Compute bloom filter from tag IDs first
    std::uint64_t bloom = 0;
    for (auto tagId : tags.ids) { bloom |= (std::uint64_t{1} << (tagId.value() & kBloomBitMask)); }

    return TrackHotHeader{
      .tagBloom = bloom,
      .artistId = artistId,
      .albumId = albumId,
      .genreId = genreId,
      .albumArtistId = albumArtistId,
      .year = metadata.year,
      .codecId = property.codecId,
      .bitDepth = property.bitDepth,
      .titleOffset = 0,
      .titleLen = 0,
      .tagsOffset = 0,
      .rating = property.rating,
      .tagCount = static_cast<std::uint8_t>(tags.ids.size()),
    };
  }

  TrackColdHeader TrackRecord::coldHeader() const
  {
    return TrackColdHeader{
      .fileSize = cold.fileSize,
      .mtime = cold.mtime,
      .durationMs = property.durationMs,
      .sampleRate = property.sampleRate,
      .coverArtId = cold.coverArtId,
      .bitrate = property.bitrate,
      .trackNumber = cold.trackNumber,
      .totalTracks = cold.totalTracks,
      .discNumber = cold.discNumber,
      .totalDiscs = cold.totalDiscs,
      .uriOffset = 0,
      .uriLen = 0,
      .channels = property.channels,
    };
  }

  std::vector<std::byte> TrackRecord::serializeHot() const
  {
    std::vector<std::byte> data;

    // Build header
    auto h = hotHeader();

    // Calculate offsets - tags first (hot data for filtering)
    std::uint16_t tagsOffset = 0;
    std::uint16_t titleOffset = static_cast<std::uint16_t>(tags.ids.size() * sizeof(std::uint32_t));

    h.titleOffset = titleOffset;
    h.titleLen = static_cast<std::uint16_t>(metadata.title.size());
    h.tagsOffset = tagsOffset;

    // Write header
    auto headerBytes = utility::asBytes(h);
    data.insert(data.end(), headerBytes.begin(), headerBytes.end());

    // Write tags first: 4-byte tag IDs
    for (auto tagId : tags.ids)
    {
      auto idBytes = utility::asBytes(tagId);
      data.insert(data.end(), idBytes.begin(), idBytes.end());
    }

    // Write title
    auto titleBytes = utility::asBytes(metadata.title);
    data.insert(data.end(), titleBytes.begin(), titleBytes.end());
    data.push_back(static_cast<std::byte>('\0'));

    return data;
  }

  std::vector<std::byte> TrackRecord::serializeCold() const
  {
    return encodeColdData(coldHeader(), customMeta, cold.uri);
  }

} // namespace rs::core
