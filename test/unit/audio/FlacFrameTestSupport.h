// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ao::audio::test
{
  // Independently authored STREAMINFO declaring 48000 Hz and 16-sample
  // fixed blocks. Defaults preserve the original stereo 16-bit fixture bytes.
  std::vector<std::uint8_t> buildFlacStreaminfoBytes(std::uint64_t totalSamples = 32,
                                                     std::uint8_t channels = 2,
                                                     std::uint8_t bitsPerSample = 16);

  // Appends a VORBIS_COMMENT block to metadata-only bytes, updating last-block
  // flags. Length-prefixed strings preserve embedded NUL bytes.
  void appendFlacCommentBlock(std::vector<std::uint8_t>& metadataBytes, std::span<std::string const> comments);

  // Appends an internally arbitrary VC payload in a complete metadata block.
  // Only outer block lengths and last-block flags are maintained/validated.
  void appendFlacRawCommentBlock(std::vector<std::uint8_t>& metadataBytes, std::span<std::uint8_t const> payload);

  // Builds one verbatim 16-sample frame carrying constant per-channel marker
  // values, with independently authored CRC-8 and CRC-16, so a decoding or
  // positioning regression is visible in the delivered marker samples.
  std::vector<std::uint8_t> buildFlacVerbatimFrameBytes(std::uint32_t frameIndex,
                                                        std::uint32_t sampleRate,
                                                        std::uint8_t channels,
                                                        std::uint8_t bitsPerSample,
                                                        std::span<std::int32_t const> markers);
} // namespace ao::audio::test
