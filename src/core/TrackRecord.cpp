// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/TrackRecord.h>
#include <rs/utility/ByteView.h>

#include <cstring>

namespace rs::core
{
  // Bloom filter uses 5 bits per tag (bit mask 31 = 0x1F)
  constexpr std::uint32_t kBloomBitMask = 31;

  TrackRecord::TrackRecord(TrackView const& view, DictionaryStore const& dict)
  {
    if (!view.isHotValid()) { return; }

    // Copy property fields from unified proxy
    auto prop = view.property();
    property.codecId = prop.codecId();
    property.bitDepth = prop.bitDepth();
    property.rating = prop.rating();
    property.fileSize = prop.fileSize();
    property.mtime = prop.mtime();
    property.durationMs = prop.durationMs();
    property.sampleRate = prop.sampleRate();
    property.bitrate = prop.bitrate();
    property.channels = prop.channels();

    // Get metadata from unified proxy
    auto meta = view.metadata();
    metadata.title = std::string{meta.title()};
    metadata.year = meta.year();

    // Get uri from property proxy
    metadata.uri = std::string{view.property().uri()};

    // Resolve dictionary IDs to strings
    if (meta.artistId() > 0) { metadata.artist = std::string{dict.get(meta.artistId())}; }
    if (meta.albumId() > 0) { metadata.album = std::string{dict.get(meta.albumId())}; }
    if (meta.albumArtistId() > 0) { metadata.albumArtist = std::string{dict.get(meta.albumArtistId())}; }
    if (meta.genreId() > 0) { metadata.genre = std::string{dict.get(meta.genreId())}; }

    // Get tag IDs from hot payload
    auto tagProxy = view.tags();
    auto tagCount = tagProxy.count();
    for (std::uint8_t i = 0; i < tagCount; ++i)
    {
      auto tagId = tagProxy.id(i);
      tags.ids.push_back(tagId);
    }

    // Copy remaining cold fields (uri already done above)
    metadata.uri = std::string{prop.uri()};
    property.fileSize = prop.fileSize();
    property.mtime = prop.mtime();
    metadata.coverArtId = meta.coverArtId();
    metadata.trackNumber = meta.trackNumber();
    metadata.totalTracks = meta.totalTracks();
    metadata.discNumber = meta.discNumber();
    metadata.totalDiscs = meta.totalDiscs();

    // Load custom meta from cold view
    for (auto const& [k, v] : view.custom()) { custom.pairs.emplace_back(std::string{k}, std::string{v}); }
  }

  TrackHotHeader TrackRecord::hotHeader() const
  {
    // Compute bloom filter from tag IDs first
    std::uint32_t bloom = 0;
    for (auto tagId : tags.ids) { bloom |= (std::uint32_t{1} << (tagId.value() & kBloomBitMask)); }

    return TrackHotHeader{
      .tagBloom = bloom,
      .artistId = artistId,
      .albumId = albumId,
      .genreId = genreId,
      .albumArtistId = albumArtistId,
      .year = metadata.year,
      .codecId = property.codecId,
      .bitDepth = property.bitDepth,
      .titleLen = static_cast<std::uint16_t>(metadata.title.size()),
      .tagLen = static_cast<std::uint16_t>(tags.ids.size() * sizeof(DictionaryId)),
      .rating = property.rating,
      .padding = std::byte{0},
    };
  }

  TrackColdHeader TrackRecord::coldHeader() const
  {
    auto [fileSizeLo, fileSizeHi] = utility::splitInt64(property.fileSize);
    auto [mtimeLo, mtimeHi] = utility::splitInt64(property.mtime);

    return TrackColdHeader{
      .fileSizeLo = fileSizeLo,
      .fileSizeHi = fileSizeHi,
      .mtimeLo = mtimeLo,
      .mtimeHi = mtimeHi,
      .durationMs = property.durationMs,
      .sampleRate = property.sampleRate,
      .coverArtId = metadata.coverArtId,
      .bitrate = property.bitrate,
      .trackNumber = metadata.trackNumber,
      .totalTracks = metadata.totalTracks,
      .discNumber = metadata.discNumber,
      .totalDiscs = metadata.totalDiscs,
      .customLen = 0,
      .uriLen = 0,
      .channels = property.channels,
      .padding = std::byte{0},
    };
  }

  std::vector<std::byte> TrackRecord::serializeHot() const
  {
    std::vector<std::byte> data;

    // Build header (titleOffset and tagsOffset are now computed, not stored)
    auto h = hotHeader();

    // Write header
    auto headerBytes = utility::asBytes(h);
    data.insert_range(data.end(), headerBytes);

    // Write tags first: 4-byte tag IDs (at offset sizeof(TrackHotHeader))
    for (auto tagId : tags.ids)
    {
      auto idBytes = utility::asBytes(tagId);
      data.insert_range(data.end(), idBytes);
    }

    // Write title (at offset sizeof(TrackHotHeader) + tagLen)
    auto titleBytes = utility::asBytes(metadata.title);
    data.insert_range(data.end(), titleBytes);
    data.push_back(static_cast<std::byte>('\0'));

    // Pad to 4-byte alignment
    while (data.size() % 4 != 0) { data.push_back(std::byte{0}); }

    return data;
  }

  std::vector<std::byte> TrackRecord::serializeCold() const
  {
    std::vector<std::byte> result;

    // Calculate custom meta size
    std::uint16_t customSize = 0;
    for (auto const& [key, value] : custom.pairs)
    {
      customSize += sizeof(std::uint16_t) * 2; // keyLen + valueLen
      customSize += static_cast<std::uint16_t>(key.size() + value.size());
    }

    std::uint16_t uriLen = static_cast<std::uint16_t>(metadata.uri.size());

    // Reserve space
    result.reserve(sizeof(TrackColdHeader) + customSize + uriLen + 1);

    // Build header with correct offsets
    TrackColdHeader hdr = coldHeader();
    hdr.customLen = customSize;
    hdr.uriLen = uriLen;

    // Write fixed header
    result.insert_range(result.end(), utility::asBytes(hdr));

    // Write custom key-value pairs
    for (auto const& [key, value] : custom.pairs)
    {
      auto keyLen = static_cast<std::uint16_t>(key.size());
      result.insert_range(result.end(), utility::asBytes(keyLen));

      auto valueLen = static_cast<std::uint16_t>(value.size());
      result.insert_range(result.end(), utility::asBytes(valueLen));

      result.insert_range(result.end(), utility::asBytes(key));
      result.insert_range(result.end(), utility::asBytes(value));
    }

    // Write uri (null-terminated)
    result.insert_range(result.end(), utility::asBytes(metadata.uri));
    result.push_back(std::byte{'\0'});

    // Pad to 4-byte alignment
    while (result.size() % 4 != 0) { result.push_back(std::byte{0}); }

    return result;
  }

} // namespace rs::core
