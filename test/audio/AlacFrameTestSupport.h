// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ao::audio::test
{
  struct AlacFrameTestOptions final
  {
    std::uint8_t channels = 2;
    std::uint8_t precisionBits = 16;
    std::uint32_t packetCount = 1;
    std::vector<std::uint8_t> sampleEntryPrefix{};
    std::vector<std::uint8_t> sampleEntrySuffix{};
    std::vector<std::uint8_t> cookieSuffix{};
  };

  std::vector<std::uint8_t> makeAlacChannelLayoutAtom(std::uint32_t tag,
                                                      std::uint32_t bitmap = 0,
                                                      std::span<std::uint32_t const> labels = {});

  // Independently authored uncompressed ALAC in a ready MP4: one to four
  // packets of sixteen 48 kHz frames, one to eight slots, and 16/20/24/32-bit
  // literal markers. Sibling bytes surround the inner alac atom; cookieSuffix
  // belongs inside its declared extent. No channel layouts are interpreted.
  // The default preserves the original EngineStereoReuseTest fixture bytes.
  std::vector<std::uint8_t> buildAlacUncompressedFileBytes(std::uint8_t channels = 2);
  std::vector<std::uint8_t> buildAlacUncompressedFileBytes(AlacFrameTestOptions const& options);
} // namespace ao::audio::test
