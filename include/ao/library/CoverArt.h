// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <cstdint>

namespace ao::library
{
  /**
   * PictureType - APIC/FLAC picture type enum.
   * Values are identical to the ID3v2 APIC picture type byte (0x00-0x14)
   * and the FLAC METADATA_BLOCK_PICTURE type field.
   */
  enum class PictureType : std::uint8_t
  {
    Other = 0x00,
    FileIcon = 0x01, // 32x32 PNG icon
    OtherIcon = 0x02,
    FrontCover = 0x03,
    BackCover = 0x04,
    LeafletPage = 0x05,
    Media = 0x06, // e.g. label side of CD
    LeadArtist = 0x07,
    Artist = 0x08,
    Conductor = 0x09,
    Band = 0x0A,
    Composer = 0x0B,
    Lyricist = 0x0C,
    RecordingLocation = 0x0D,
    DuringRecording = 0x0E,
    DuringPerformance = 0x0F,
    VideoCapture = 0x10,
    BrightColouredFish = 0x11,
    Illustration = 0x12,
    BandLogo = 0x13,
    PublisherLogo = 0x14,
  };

  /**
   * CoverArt - typed cover art stored as a ResourceStore reference.
   */
  struct CoverArt
  {
    ResourceId resourceId{};
    PictureType type = PictureType::FrontCover;
  };
} // namespace ao::library
