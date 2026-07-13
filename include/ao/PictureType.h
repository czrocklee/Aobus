// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao
{
  /**
   * APIC/FLAC picture role.
   *
   * Values are identical to the ID3v2 APIC picture-type byte and the FLAC
   * METADATA_BLOCK_PICTURE type field.
   */
  enum class PictureType : std::uint8_t
  {
    Other = 0x00,
    FileIcon = 0x01,
    OtherIcon = 0x02,
    FrontCover = 0x03,
    BackCover = 0x04,
    LeafletPage = 0x05,
    Media = 0x06,
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
} // namespace ao
