// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/library/RecordingDate.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ratio>
#include <type_traits>

namespace ao::library
{
  /**
   * Storage representation for a track's duration: a millisecond span backed by a 32-bit
   * signed integer. Sized to fit the on-disk header (4 bytes, same byte layout as the
   * previous std::uint32_t); the signed int32 range (~24.8 days) is far beyond any real
   * track. Widens implicitly and losslessly to std::chrono::milliseconds at use sites.
   */
  using TrackDuration = std::chrono::duration<std::int32_t, std::milli>;
  /**
   * TrackHotHeader - POD struct for hot track storage.
   * Hot fields are used for fast filtering/sorting operations.
   *
   * Layout uses strictly descending member sizes (4→2→1) for natural alignment.
   * Total size: 36 bytes with 4-byte alignment. The members end exactly on a 4-byte
   * boundary (7×4 + 3×2 + 2×1 = 36), so there is no internal or trailing padding and
   * every byte is a real field — on-disk serialization is fully deterministic.
   *
   * Layout:
   *   ┌─────────────────────────────────────┐  ← hot data begin
   *   │        TrackHotHeader (36B)         │
   *   │  tagBloom, sampleRate,              │
   *   │  artistId, albumId,                 │
   *   │  genreId, albumArtistId,            │
   *   │  composerId, year,                  │
   *   │  titleLength, tagLength,            │
   *   │  bitDepth, codec                    │
   *   ├─────────────────────────────────────┤  ← tags begin = header + sizeof(header)
   *   │  tag ID 1 (4B)                      │
   *   │  tag ID 2 (4B)                      │
   *   │  ... (tagLength bytes total)        │
   *   ├─────────────────────────────────────┤  ← title begin = tags begin + tagLength
   *   │  title... (titleLength bytes)       │
   *   │  '\0'                               │
   *   └─────────────────────────────────────┘  ← hot data end
   */
  struct TrackHotHeader final
  {
    // 4-byte section
    std::uint32_t tagBloom{};     // Bloom filter for tags (32-bit)
    DictionaryId artistId{};      // Dictionary ID for artist
    DictionaryId albumId{};       // Dictionary ID for album
    DictionaryId genreId{};       // Dictionary ID for genre
    DictionaryId albumArtistId{}; // Dictionary ID for album artist
    DictionaryId composerId{};    // Dictionary ID for composer
    SampleRate sampleRate{};      // Sample rate in Hz

    // 2-byte section
    std::uint16_t year{};        // Release year
    std::uint16_t titleLength{}; // Length of title string
    std::uint16_t tagLength{};   // Length of tags blob in bytes

    // 1-byte section
    BitDepth bitDepth{}; // Bits per sample
    AudioCodec codec{};  // Audio codec
  };

  // Binary layout constants
  constexpr std::size_t kTrackHotHeaderSize = 36;
  constexpr std::size_t kTrackHotHeaderAlignment = 4;

  static_assert(sizeof(TrackHotHeader) == kTrackHotHeaderSize, "TrackHotHeader must be exactly 36 bytes");
  static_assert(alignof(TrackHotHeader) == kTrackHotHeaderAlignment, "TrackHotHeader must have 4-byte alignment");
  static_assert(std::has_unique_object_representations_v<TrackHotHeader>, "TrackHotHeader must have no padding");
  static_assert(offsetof(TrackHotHeader, tagBloom) == 0);
  static_assert(offsetof(TrackHotHeader, artistId) ==
                offsetof(TrackHotHeader, tagBloom) + sizeof(TrackHotHeader::tagBloom));
  static_assert(offsetof(TrackHotHeader, albumId) ==
                offsetof(TrackHotHeader, artistId) + sizeof(TrackHotHeader::artistId));
  static_assert(offsetof(TrackHotHeader, genreId) ==
                offsetof(TrackHotHeader, albumId) + sizeof(TrackHotHeader::albumId));
  static_assert(offsetof(TrackHotHeader, albumArtistId) ==
                offsetof(TrackHotHeader, genreId) + sizeof(TrackHotHeader::genreId));
  static_assert(offsetof(TrackHotHeader, composerId) ==
                offsetof(TrackHotHeader, albumArtistId) + sizeof(TrackHotHeader::albumArtistId));
  static_assert(offsetof(TrackHotHeader, sampleRate) ==
                offsetof(TrackHotHeader, composerId) + sizeof(TrackHotHeader::composerId));
  static_assert(offsetof(TrackHotHeader, year) ==
                offsetof(TrackHotHeader, sampleRate) + sizeof(TrackHotHeader::sampleRate));
  static_assert(offsetof(TrackHotHeader, titleLength) == offsetof(TrackHotHeader, year) + sizeof(TrackHotHeader::year));
  static_assert(offsetof(TrackHotHeader, tagLength) ==
                offsetof(TrackHotHeader, titleLength) + sizeof(TrackHotHeader::titleLength));
  static_assert(offsetof(TrackHotHeader, bitDepth) ==
                offsetof(TrackHotHeader, tagLength) + sizeof(TrackHotHeader::tagLength));
  static_assert(offsetof(TrackHotHeader, codec) ==
                offsetof(TrackHotHeader, bitDepth) + sizeof(TrackHotHeader::bitDepth));

  /** Storage representation for one typed cover-art resource reference. */
  struct CoverArtEntry final
  {
    ResourceId id{};     // 4 bytes - Links to the blob in ResourceStore
    std::uint8_t type{}; // 1 byte - PictureType enum value
    std::array<std::uint8_t, 3> reserved{};
  };

  static_assert(sizeof(CoverArtEntry) == 8, "CoverArtEntry must be exactly 8 bytes");
  static_assert(alignof(CoverArtEntry) == 4, "CoverArtEntry must have 4-byte alignment");
  static_assert(std::has_unique_object_representations_v<CoverArtEntry>, "CoverArtEntry must have no padding");
  static_assert(offsetof(CoverArtEntry, id) == 0);
  static_assert(offsetof(CoverArtEntry, type) == offsetof(CoverArtEntry, id) + sizeof(CoverArtEntry::id));
  static_assert(offsetof(CoverArtEntry, reserved) == offsetof(CoverArtEntry, type) + sizeof(CoverArtEntry::type));

  enum class TrackColdBlockSlot : std::uint8_t
  {
    CoverArt = 0,
    Work = 1,
    CustomMetadata = 2,
    Performance = 3,
    Segment = 4,
  };

  constexpr std::size_t kTrackColdKnownBlockSlotCount = 5;
  constexpr std::size_t kTrackColdBlockSlotCount = 5;

  constexpr std::size_t trackColdBlockSlotIndex(TrackColdBlockSlot slot) noexcept
  {
    return static_cast<std::size_t>(slot);
  }

  constexpr std::uint16_t kMaxTrackSegmentOrdinal = 99;

  struct TrackSegment final
  {
    std::uint64_t startFrame{};
    std::uint64_t frameCount{};
    std::uint16_t ordinal{};

    bool operator==(TrackSegment const&) const = default;
  };

  constexpr std::optional<TrackDuration> segmentDuration(std::uint64_t frameCount, SampleRate sampleRate) noexcept
  {
    auto const rate = sampleRate.raw();

    if (rate == 0 || frameCount / rate > static_cast<std::uint64_t>(TrackDuration::max().count()) / 1000)
    {
      return std::nullopt;
    }

    auto const milliseconds = ((frameCount / rate) * 1000) + (((frameCount % rate) * 1000) / rate);

    if (milliseconds > static_cast<std::uint64_t>(TrackDuration::max().count()))
    {
      return std::nullopt;
    }

    return TrackDuration{static_cast<std::int32_t>(milliseconds)};
  }

  struct TrackSegmentBlock final
  {
    static constexpr std::size_t kSize = 20;
    static constexpr std::size_t kFrameCountHiOffset = 12;
    static constexpr std::size_t kReservedOffset = 18;

    std::uint32_t startFrameLo{};
    std::uint32_t startFrameHi{};
    std::uint32_t frameCountLo{};
    std::uint32_t frameCountHi{};
    std::uint16_t ordinal{};
    std::uint16_t reserved{};
  };

  static_assert(sizeof(TrackSegmentBlock) == TrackSegmentBlock::kSize);
  static_assert(alignof(TrackSegmentBlock) == 4);
  static_assert(std::has_unique_object_representations_v<TrackSegmentBlock>);
  static_assert(offsetof(TrackSegmentBlock, startFrameLo) == 0);
  static_assert(offsetof(TrackSegmentBlock, startFrameHi) == 4);
  static_assert(offsetof(TrackSegmentBlock, frameCountLo) == 8);
  static_assert(offsetof(TrackSegmentBlock, frameCountHi) == TrackSegmentBlock::kFrameCountHiOffset);
  static_assert(offsetof(TrackSegmentBlock, ordinal) == 16);
  static_assert(offsetof(TrackSegmentBlock, reserved) == TrackSegmentBlock::kReservedOffset);

  struct TrackWorkBlock final
  {
    DictionaryId workId{};
    DictionaryId movementId{};
    std::uint16_t movementNumber{};
    std::uint16_t movementTotal{};
  };

  constexpr std::size_t kTrackWorkBlockSize = 12;

  static_assert(sizeof(TrackWorkBlock) == kTrackWorkBlockSize);
  static_assert(alignof(TrackWorkBlock) == 4);
  static_assert(std::has_unique_object_representations_v<TrackWorkBlock>);
  static_assert(offsetof(TrackWorkBlock, workId) == 0);
  static_assert(offsetof(TrackWorkBlock, movementId) ==
                offsetof(TrackWorkBlock, workId) + sizeof(TrackWorkBlock::workId));
  static_assert(offsetof(TrackWorkBlock, movementNumber) ==
                offsetof(TrackWorkBlock, movementId) + sizeof(TrackWorkBlock::movementId));
  static_assert(offsetof(TrackWorkBlock, movementTotal) ==
                offsetof(TrackWorkBlock, movementNumber) + sizeof(TrackWorkBlock::movementNumber));

  /** Fixed prefix followed by credit entries in nondecreasing kind order. */
  struct alignas(4) TrackPerformanceBlock final
  {
    RecordingDate recordingDate{};
  };

  /** One credit; `kind` holds a `CreditKind` value. */
  struct TrackCreditEntry final
  {
    DictionaryId nameId{};
    DictionaryId roleId{};
    std::uint8_t kind{};
    std::array<std::uint8_t, 3> reserved{};
  };

  static_assert(std::is_standard_layout_v<RecordingDate>);
  static_assert(std::is_trivially_copyable_v<RecordingDate>);
  static_assert(sizeof(RecordingDate) == 4);
  static_assert(offsetof(RecordingDate, year) == 0);
  static_assert(offsetof(RecordingDate, month) == 2);
  static_assert(offsetof(RecordingDate, day) == 3);
  static_assert(std::is_standard_layout_v<TrackPerformanceBlock>);
  static_assert(std::is_trivially_copyable_v<TrackPerformanceBlock>);
  constexpr std::size_t kTrackPerformanceBlockSize = 4;
  static_assert(sizeof(TrackPerformanceBlock) == kTrackPerformanceBlockSize);
  static_assert(alignof(TrackPerformanceBlock) == 4);
  static_assert(offsetof(TrackPerformanceBlock, recordingDate) == 0);
  static_assert(std::has_unique_object_representations_v<TrackPerformanceBlock>);
  static_assert(std::is_standard_layout_v<TrackCreditEntry>);
  static_assert(std::is_trivially_copyable_v<TrackCreditEntry>);
  constexpr std::size_t kTrackCreditEntrySize = 12;
  static_assert(sizeof(TrackCreditEntry) == kTrackCreditEntrySize);
  static_assert(alignof(TrackCreditEntry) == 4);
  static_assert(offsetof(TrackCreditEntry, nameId) == 0);
  static_assert(offsetof(TrackCreditEntry, roleId) ==
                offsetof(TrackCreditEntry, nameId) + sizeof(TrackCreditEntry::nameId));
  static_assert(offsetof(TrackCreditEntry, kind) ==
                offsetof(TrackCreditEntry, roleId) + sizeof(TrackCreditEntry::roleId));
  static_assert(offsetof(TrackCreditEntry, reserved) ==
                offsetof(TrackCreditEntry, kind) + sizeof(TrackCreditEntry::kind));
  static_assert(std::has_unique_object_representations_v<TrackCreditEntry>);

  struct CustomMetadataBlockHeader final
  {
    std::uint16_t entryCount{};
    std::uint16_t valueOffset{};
    std::uint16_t payloadLength{};
    std::uint16_t reserved{};
  };

  static_assert(sizeof(CustomMetadataBlockHeader) == 8, "CustomMetadataBlockHeader must be exactly 8 bytes");
  static_assert(alignof(CustomMetadataBlockHeader) <= 4, "CustomMetadataBlockHeader must fit cold record alignment");
  static_assert(std::has_unique_object_representations_v<CustomMetadataBlockHeader>);
  static_assert(offsetof(CustomMetadataBlockHeader, entryCount) == 0);
  static_assert(offsetof(CustomMetadataBlockHeader, valueOffset) ==
                offsetof(CustomMetadataBlockHeader, entryCount) + sizeof(CustomMetadataBlockHeader::entryCount));
  static_assert(offsetof(CustomMetadataBlockHeader, payloadLength) ==
                offsetof(CustomMetadataBlockHeader, valueOffset) + sizeof(CustomMetadataBlockHeader::valueOffset));
  static_assert(offsetof(CustomMetadataBlockHeader, reserved) ==
                offsetof(CustomMetadataBlockHeader, payloadLength) + sizeof(CustomMetadataBlockHeader::payloadLength));

  /**
   * CustomMetadataEntry - Binary entry for custom key-value metadata in the custom block payload.
   * 8 bytes total, 4-byte aligned.
   */
  struct CustomMetadataEntry final
  {
    DictionaryId keyId{};          // 4 bytes - Links to the key string in DictionaryStore
    std::uint16_t valueOffset = 0; // 2 bytes - byte offset from custom payload start to value
    std::uint16_t valueLength = 0; // 2 bytes - value length in bytes
  };

  static_assert(sizeof(CustomMetadataEntry) == 8, "CustomMetadataEntry must be exactly 8 bytes");
  static_assert(alignof(CustomMetadataEntry) == 4, "CustomMetadataEntry must have 4-byte alignment");
  static_assert(std::has_unique_object_representations_v<CustomMetadataEntry>);
  static_assert(offsetof(CustomMetadataEntry, keyId) == 0);
  static_assert(offsetof(CustomMetadataEntry, valueOffset) ==
                offsetof(CustomMetadataEntry, keyId) + sizeof(CustomMetadataEntry::keyId));
  static_assert(offsetof(CustomMetadataEntry, valueLength) ==
                offsetof(CustomMetadataEntry, valueOffset) + sizeof(CustomMetadataEntry::valueOffset));

  /**
   * TrackColdHeader - POD struct for cold track fixed fields and extension block offsets.
   * Layout uses strictly descending member sizes (4→2→1) for natural alignment.
   *
   * Cold fixed fields are those not used in high-frequency filter/sort operations:
   *   - duration, bitrate, channels: audio properties
   *   - trackNumber, trackTotal, discNumber, discTotal: display only
   *   - extension block area: cover art, work, custom metadata, performance
   *   - uri: playback path, stored after the block area
   *
   * Total size: 32 bytes with 4-byte alignment. The whole record, header
   * included, is at most kTrackColdRecordSizeLimit bytes.
   *
   * Layout:
   *   ┌─────────────────────────────────────┐  ← cold data begin
   *   │        TrackColdHeader (32B)        │
   *   │  duration, bitrate,                 │
   *   │  trackNumber, trackTotal,           │
   *   │  discNumber, discTotal,             │
   *   │  blockOffsets[5], uriOffset,        │
   *   │  uriLength, channels, reserved8     │
   *   ├─────────────────────────────────────┤  ← first present block offset
   *   │  block payload                      │
   *   │  ...                                │
   *   ├─────────────────────────────────────┤  ← uriOffset
   *   │  uri data... (uriLength bytes)      │
   *   └─────────────────────────────────────┘  ← cold data end
   */
  struct TrackColdHeader final
  {
    // 4-byte section
    TrackDuration duration{}; // Track duration (millisecond span)
    Bitrate bitrate{};        // Bitrate in bps

    // 2-byte section
    std::uint16_t trackNumber{};                                        // Track number
    std::uint16_t trackTotal{};                                         // Total tracks in album
    std::uint16_t discNumber{};                                         // Disc number
    std::uint16_t discTotal{};                                          // Total discs in album
    std::array<std::uint16_t, kTrackColdBlockSlotCount> blockOffsets{}; // 0 means absent
    std::uint16_t uriOffset{};                                          // Byte offset from header start to uri string
    std::uint16_t uriLength{};                                          // Length of URI string

    // 1-byte section
    Channels channels{}; // Number of audio channels
    std::uint8_t reserved8{};
  };

  // Binary layout constants
  constexpr std::size_t kTrackColdHeaderSize = 32;
  constexpr std::size_t kTrackColdHeaderAlignment = 4;
  // Covers the header, every block, and the URI. Records are four-byte
  // aligned, so the largest stored record is 65,532 bytes.
  constexpr std::size_t kTrackColdRecordSizeLimit = 65'535;

  static_assert(kTrackColdRecordSizeLimit == std::numeric_limits<decltype(TrackColdHeader::uriOffset)>::max(),
                "The cold record limit must match the range of its 16-bit offsets");
  static_assert(sizeof(TrackColdHeader) == kTrackColdHeaderSize, "TrackColdHeader must be exactly 32 bytes");
  static_assert(alignof(TrackColdHeader) == kTrackColdHeaderAlignment, "TrackColdHeader must have 4-byte alignment");
  static_assert(std::has_unique_object_representations_v<TrackColdHeader>, "TrackColdHeader must have no padding");
  static_assert(offsetof(TrackColdHeader, duration) == 0);
  static_assert(offsetof(TrackColdHeader, bitrate) ==
                offsetof(TrackColdHeader, duration) + sizeof(TrackColdHeader::duration));
  static_assert(offsetof(TrackColdHeader, trackNumber) ==
                offsetof(TrackColdHeader, bitrate) + sizeof(TrackColdHeader::bitrate));
  static_assert(offsetof(TrackColdHeader, trackTotal) ==
                offsetof(TrackColdHeader, trackNumber) + sizeof(TrackColdHeader::trackNumber));
  static_assert(offsetof(TrackColdHeader, discNumber) ==
                offsetof(TrackColdHeader, trackTotal) + sizeof(TrackColdHeader::trackTotal));
  static_assert(offsetof(TrackColdHeader, discTotal) ==
                offsetof(TrackColdHeader, discNumber) + sizeof(TrackColdHeader::discNumber));
  static_assert(offsetof(TrackColdHeader, blockOffsets) ==
                offsetof(TrackColdHeader, discTotal) + sizeof(TrackColdHeader::discTotal));
  static_assert(offsetof(TrackColdHeader, uriOffset) ==
                offsetof(TrackColdHeader, blockOffsets) + sizeof(TrackColdHeader::blockOffsets));
  static_assert(offsetof(TrackColdHeader, uriLength) ==
                offsetof(TrackColdHeader, uriOffset) + sizeof(TrackColdHeader::uriOffset));
  static_assert(offsetof(TrackColdHeader, channels) ==
                offsetof(TrackColdHeader, uriLength) + sizeof(TrackColdHeader::uriLength));
  static_assert(offsetof(TrackColdHeader, reserved8) ==
                offsetof(TrackColdHeader, channels) + sizeof(TrackColdHeader::channels));

  static_assert(sizeof(DictionaryId) == 4, "DictionaryId must stay 4 bytes");
  static_assert(alignof(DictionaryId) == 4, "DictionaryId must stay 4-byte aligned");
  static_assert(sizeof(ResourceId) == 4, "ResourceId must stay 4 bytes");
  static_assert(alignof(ResourceId) == 4, "ResourceId must stay 4-byte aligned");
  static_assert(sizeof(Bitrate) == 4, "Bitrate must stay 4 bytes");
  static_assert(alignof(Bitrate) == 4, "Bitrate must stay 4-byte aligned");
  static_assert(sizeof(Channels) == 1, "Channels must stay 1 byte");
  static_assert(alignof(Channels) == 1, "Channels must stay 1-byte aligned");
  static_assert(sizeof(TrackDuration) == 4, "TrackDuration must stay 4 bytes");
  static_assert(alignof(TrackDuration) == 4, "TrackDuration must stay 4-byte aligned");
} // namespace ao::library
