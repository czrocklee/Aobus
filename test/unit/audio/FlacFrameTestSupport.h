// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ao::audio::test
{
  // Independently authored FLAC byte fixtures shared by the decoder tests: a
  // STREAMINFO block declaring 48000 Hz, 2 channels, and 16 bits per sample,
  // plus fixed-blocksize frames of 16 verbatim samples each. The declared
  // total sample count stays an argument because the stream-end contract
  // differs between known and unknown totals; it defaults to the 32-sample
  // stream used by the format-stability fixtures.
  std::vector<std::uint8_t> buildFlacStreaminfoBytes(std::uint64_t totalSamples = 32);

  // Builds one verbatim 16-sample frame carrying constant per-channel marker
  // values, with independently authored CRC-8 and CRC-16, so a decoding or
  // positioning regression is visible in the delivered marker samples.
  std::vector<std::uint8_t> buildFlacVerbatimFrameBytes(std::uint32_t frameIndex,
                                                        std::uint32_t sampleRate,
                                                        std::uint8_t channels,
                                                        std::uint8_t bitsPerSample,
                                                        std::span<std::int32_t const> markers);
} // namespace ao::audio::test
