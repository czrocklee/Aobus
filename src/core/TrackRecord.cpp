// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/TrackRecord.h>
#include <rs/utility/ByteView.h>

#include <algorithm>
#include <cstring>
#include <functional>

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

    // Load custom meta from cold view (keys are DictionaryIds, need to resolve to string via dict)
    for (auto const& [dictId, value] : view.custom())
    {
      auto key = dict.get(dictId);
      custom.pairs.emplace_back(std::string{key}, std::string{value});
    }
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
      .customCount = 0,
      .uriOffset = 0,
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

  std::vector<std::byte> TrackRecord::serializeCold(DictionaryStore const& dict) const
  {
    return serializeCold([&dict](std::string_view key) { return dict.getId(key); });
  }

  std::vector<std::byte> TrackRecord::serializeCold(std::function<DictionaryId(std::string_view)> resolveKey) const
  {
    // Resolve keys to DictionaryIds
    std::vector<std::pair<DictionaryId, std::string>> resolvedPairs;
    resolvedPairs.reserve(custom.pairs.size());
    for (auto const& [key, value] : custom.pairs)
    {
      auto dictId = resolveKey(key);
      resolvedPairs.emplace_back(dictId, value);
    }

    // Sort by dictId for binary search
    std::ranges::sort(resolvedPairs, [](auto const& a, auto const& b) { return a.first < b.first; });

    constexpr std::size_t kEntrySize = 8; // dictId(4) + offset(2) + len(2)
    std::size_t entryCount = resolvedPairs.size();
    std::size_t totalValueSize = 0;
    for (auto const& [_, value] : resolvedPairs) { totalValueSize += value.size(); }

    std::uint16_t uriLen = static_cast<std::uint16_t>(metadata.uri.size());

    std::vector<std::byte> result;
    result.reserve(sizeof(TrackColdHeader) + entryCount * kEntrySize + totalValueSize + uriLen + 4);

    // Build header
    TrackColdHeader hdr = coldHeader();
    hdr.customCount = static_cast<std::uint16_t>(entryCount);
    hdr.uriOffset = static_cast<std::uint16_t>(sizeof(TrackColdHeader) + entryCount * kEntrySize + totalValueSize);
    hdr.uriLen = uriLen;
    result.insert_range(result.end(), utility::asBytes(hdr));

    // Value data starts after header + entries
    std::size_t valueOffset = sizeof(TrackColdHeader) + entryCount * kEntrySize;

    // Write entries with calculated offsets
    for (auto const& [dictId, value] : resolvedPairs)
    {
      auto valueLen = static_cast<std::uint16_t>(value.size());
      result.insert_range(result.end(), utility::asBytes(dictId.value()));
      result.insert_range(result.end(), utility::asBytes(static_cast<std::uint16_t>(valueOffset)));
      result.insert_range(result.end(), utility::asBytes(valueLen));
      valueOffset += valueLen;
    }

    // Write all values contiguously
    for (auto const& [_, value] : resolvedPairs)
    {
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
