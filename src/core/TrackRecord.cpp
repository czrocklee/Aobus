// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/TrackLayout.h>
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
    for (auto const& [k, v] : view.custom()) { customMeta.emplace_back(std::string{k}, std::string{v}); }
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
    data.insert(data.end(), headerBytes.begin(), headerBytes.end());

    // Write tags first: 4-byte tag IDs (at offset sizeof(TrackHotHeader))
    for (auto tagId : tags.ids)
    {
      auto idBytes = utility::asBytes(tagId);
      data.insert(data.end(), idBytes.begin(), idBytes.end());
    }

    // Write title (at offset sizeof(TrackHotHeader) + tagLen)
    auto titleBytes = utility::asBytes(metadata.title);
    data.insert(data.end(), titleBytes.begin(), titleBytes.end());
    data.push_back(static_cast<std::byte>('\0'));

    // Pad to 4-byte alignment
    while (data.size() % 4 != 0) { data.push_back(std::byte{0}); }

    return data;
  }

  std::vector<std::byte> TrackRecord::serializeCold() const
  {
    std::vector<std::byte> result;

    // Calculate custom meta size
    std::uint16_t customMetaSize = 0;
    for (auto const& [key, value] : customMeta)
    {
      customMetaSize += sizeof(std::uint16_t) * 2; // keyLen + valueLen
      customMetaSize += static_cast<std::uint16_t>(key.size() + value.size());
    }

    std::uint16_t uriLen = static_cast<std::uint16_t>(metadata.uri.size());

    // Reserve space
    result.reserve(sizeof(TrackColdHeader) + customMetaSize + uriLen + 1);

    // Build header with correct offsets
    TrackColdHeader hdr = coldHeader();
    hdr.customLen = customMetaSize;
    hdr.uriLen = uriLen;

    // Write fixed header
    auto const* headerBytes = reinterpret_cast<std::byte const*>(&hdr);
    result.insert(result.end(), headerBytes, headerBytes + sizeof(TrackColdHeader));

    // Write custom key-value pairs
    std::byte buf[sizeof(std::uint16_t)];
    for (auto const& [key, value] : customMeta)
    {
      std::uint16_t keyLen = static_cast<std::uint16_t>(key.size());
      std::memcpy(buf, &keyLen, sizeof(keyLen));
      result.insert(result.end(), buf, buf + sizeof(keyLen));

      std::uint16_t valueLen = static_cast<std::uint16_t>(value.size());
      std::memcpy(buf, &valueLen, sizeof(valueLen));
      result.insert(result.end(), buf, buf + sizeof(valueLen));

      auto const* keyBytes = reinterpret_cast<std::byte const*>(key.data());
      result.insert(result.end(), keyBytes, keyBytes + key.size());

      auto const* valueBytes = reinterpret_cast<std::byte const*>(value.data());
      result.insert(result.end(), valueBytes, valueBytes + value.size());
    }

    // Write uri (null-terminated)
    if (!metadata.uri.empty())
    {
      auto const* uriBytes = reinterpret_cast<std::byte const*>(metadata.uri.data());
      result.insert(result.end(), uriBytes, uriBytes + metadata.uri.size());
    }
    result.push_back(std::byte{'\0'});

    // Pad to 4-byte alignment
    while (result.size() % 4 != 0) { result.push_back(std::byte{0}); }

    return result;
  }

} // namespace rs::core
