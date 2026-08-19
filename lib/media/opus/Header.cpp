// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/media/opus/Header.h>

#include <ao/Error.h>
#include <ao/utility/ByteView.h>

#include <boost/endian/buffers.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace ao::media::opus
{
  namespace
  {
    constexpr std::string_view kHeadMagic = "OpusHead";
    constexpr std::string_view kTagsMagic = "OpusTags";

    // The identification packet is fixed up to the channel mapping family; a
    // family other than 0 appends a stream count, a coupled count, and one
    // mapping byte per channel.
    struct HeadLayout final
    {
      static constexpr std::size_t kSize = 19;

      using FixedSize = std::true_type;

      std::array<char, 8> magic;
      std::uint8_t version;
      std::uint8_t channels;
      boost::endian::little_uint16_buf_t preSkip;
      boost::endian::little_uint32_buf_t inputSampleRate;
      boost::endian::little_int16_buf_t outputGain;
      std::uint8_t channelMappingFamily;
    };

    static_assert(sizeof(HeadLayout) == HeadLayout::kSize, "HeadLayout should be 19 bytes");
    static_assert(alignof(HeadLayout) == 1);
    static_assert(utility::layout::kIsBinaryLayoutType<HeadLayout>);

    // Only the major half of the version is a compatibility break; a decoder
    // must accept any minor revision of version 1.
    constexpr std::uint8_t kSupportedVersionMajor = 0;

    bool hasMagic(std::span<std::byte const> packet, std::string_view magic) noexcept
    {
      return packet.size() >= magic.size() && utility::bytes::stringView(packet.first(magic.size())) == magic;
    }
  } // namespace

  Result<Head> parseHead(std::span<std::byte const> packet)
  {
    if (!hasMagic(packet, kHeadMagic) || packet.size() < HeadLayout::kSize)
    {
      return makeError(Error::Code::CorruptData, "opus identification packet is not an OpusHead packet");
    }

    auto const* const layout = utility::layout::view<HeadLayout>(packet);

    if ((layout->version >> 4U) != kSupportedVersionMajor)
    {
      return makeError(
        Error::Code::NotSupported, std::format("unsupported opus bitstream version {}", layout->version));
    }

    if (layout->channels == 0)
    {
      return makeError(Error::Code::CorruptData, "opus identification packet declares no channels");
    }

    auto head = Head{.version = layout->version,
                     .channels = layout->channels,
                     .preSkip = layout->preSkip.value(),
                     .inputSampleRate = layout->inputSampleRate.value(),
                     .outputGain = layout->outputGain.value(),
                     .channelMappingFamily = layout->channelMappingFamily};

    if (head.channelMappingFamily == kMappingFamilyMonoStereo)
    {
      if (head.channels > 2)
      {
        return makeError(
          Error::Code::CorruptData, std::format("opus mapping family 0 cannot carry {} channels", head.channels));
      }

      head.streamCount = 1;
      head.coupledStreamCount = head.channels == 2 ? 1 : 0;

      for (std::uint8_t channel = 0; channel < head.channels; ++channel)
      {
        head.channelMapping[channel] = channel;
      }

      return head;
    }

    if (head.channelMappingFamily != kMappingFamilySurround && head.channelMappingFamily != kMappingFamilyDiscrete)
    {
      return makeError(Error::Code::NotSupported,
                       std::format("unsupported opus channel mapping family {}", head.channelMappingFamily));
    }

    if (head.channelMappingFamily == kMappingFamilySurround && head.channels > kMaxSurroundChannels)
    {
      return makeError(
        Error::Code::CorruptData, std::format("opus mapping family 1 cannot carry {} channels", head.channels));
    }

    auto const mappingSize = std::size_t{2} + head.channels;

    if (packet.size() - HeadLayout::kSize < mappingSize)
    {
      return makeError(Error::Code::CorruptData, "opus identification packet omits its channel mapping table");
    }

    auto const mapping = packet.subspan(HeadLayout::kSize, mappingSize);
    head.streamCount = static_cast<std::uint8_t>(mapping[0]);
    head.coupledStreamCount = static_cast<std::uint8_t>(mapping[1]);

    // Each coupled stream decodes two channels and each remaining stream one,
    // so the decoded channel count must still be addressable by one byte.
    auto const streamChannelCount = std::size_t{head.streamCount} + head.coupledStreamCount;

    if (head.streamCount == 0 || head.coupledStreamCount > head.streamCount || streamChannelCount > kMaxChannels)
    {
      return makeError(Error::Code::CorruptData, "opus identification packet declares an inconsistent stream layout");
    }

    for (std::uint8_t channel = 0; channel < head.channels; ++channel)
    {
      auto const value = static_cast<std::uint8_t>(mapping[std::size_t{2} + channel]);

      // 255 marks a silent channel; every other index must name a decoded one.
      if (value != kMaxChannels && value >= streamChannelCount)
      {
        return makeError(Error::Code::CorruptData, "opus channel mapping table names a missing stream channel");
      }

      head.channelMapping[channel] = value;
    }

    return head;
  }

  std::optional<std::span<std::byte const>> parseTagsBody(std::span<std::byte const> packet) noexcept
  {
    if (!hasMagic(packet, kTagsMagic))
    {
      return std::nullopt;
    }

    return packet.subspan(kTagsMagic.size());
  }
} // namespace ao::media::opus
