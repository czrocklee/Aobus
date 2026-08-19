// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace ao::media::opus
{
  // Opus always decodes at 48 kHz regardless of the rate its source was
  // captured at, so this is the rate of every decoded stream.
  inline constexpr std::uint32_t kDecodedSampleRate = 48000;

  // RFC 7845 fixes the two header packets at the front of the logical
  // bitstream, so audio always begins at the third packet.
  inline constexpr std::size_t kHeadPacketIndex = 0;
  inline constexpr std::size_t kTagsPacketIndex = 1;
  inline constexpr std::size_t kFirstAudioPacketIndex = 2;

  // RFC 7845 section 12 asks a seek to resume at least 80 ms before its target
  // so the decoder converges before the first audible sample is handed out.
  inline constexpr std::int64_t kSeekPreRollFrames = 3840;

  // An Opus packet holds at most 120 ms, which one decode call must be able to
  // take and which bounds any frame count a table of contents can declare.
  inline constexpr std::int32_t kMaxPacketFrames = 5760;

  inline constexpr std::uint8_t kMaxChannels = 255;

  // Channel mapping families. Family 0 carries mono or stereo with an implicit
  // mapping; families 1 and 255 carry an explicit stream and mapping table.
  inline constexpr std::uint8_t kMappingFamilyMonoStereo = 0;
  inline constexpr std::uint8_t kMappingFamilySurround = 1;
  inline constexpr std::uint8_t kMappingFamilyDiscrete = 255;

  // RFC 7845 section 5.1.1.2 defines family 1 over the Vorbis channel orders,
  // which cover one through eight channels. Family 255 has no such table and
  // stays bounded only by kMaxChannels.
  inline constexpr std::uint8_t kMaxSurroundChannels = 8;

  /**
   * @brief The parsed OpusHead identification packet.
   *
   * Family 0 has no mapping table on the wire; parseHead() fills the equivalent
   * explicit values so every consumer can drive one multistream decoder.
   */
  struct Head final
  {
    std::uint8_t version = 0;
    std::uint8_t channels = 0;
    // Samples at kDecodedSampleRate to discard from the front of the decoder
    // output, counted from wherever decoding starts rather than from the
    // absolute timeline. A granule position minus this is a PCM position.
    std::uint16_t preSkip = 0;
    // The rate of the original source. Informational only; it never describes
    // the decoded signal.
    std::uint32_t inputSampleRate = 0;
    // Gain to apply to the decoded signal, in Q7.8 decibels. This is the same
    // unit libopus takes through OPUS_SET_GAIN, so a decoder passes it through
    // rather than converting it.
    std::int16_t outputGain = 0;
    std::uint8_t channelMappingFamily = 0;
    std::uint8_t streamCount = 0;
    std::uint8_t coupledStreamCount = 0;
    std::array<std::uint8_t, kMaxChannels> channelMapping{};
  };

  /**
   * @brief Parses an OpusHead identification packet.
   * @return The header, Error::Code::CorruptData when the magic signature or
   *         structure is unusable, or Error::Code::NotSupported for a bitstream
   *         version or channel mapping family this reader cannot describe.
   */
  Result<Head> parseHead(std::span<std::byte const> packet);

  /**
   * @brief Locates the Vorbis comment list inside an OpusTags packet.
   * @return The bytes after the magic signature, or nullopt when the packet
   *         does not carry one.
   */
  std::optional<std::span<std::byte const>> parseTagsBody(std::span<std::byte const> packet) noexcept;
} // namespace ao::media::opus
