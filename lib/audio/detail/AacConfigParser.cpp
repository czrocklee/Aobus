// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/detail/AacConfigParser.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace ao::audio::detail
{
  namespace
  {
    constexpr std::uint32_t kAacEscapeObjectType = 31U;
    constexpr std::uint32_t kAacEscapeSampleRateIndex = 15U;
    constexpr std::array<std::uint32_t, 13> kAacSampleRates =
      {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    constexpr std::array<std::uint8_t, 8> kAacChannelCounts = {0, 1, 2, 3, 4, 5, 6, 8};

    class BitReader final
    {
    public:
      explicit BitReader(std::span<std::byte const> bytes)
        : _bytes{bytes}
      {
      }

      std::optional<std::uint32_t> read(std::uint8_t bitCount) noexcept
      {
        auto value = std::uint32_t{0};

        for (std::uint8_t idx = 0; idx < bitCount; ++idx)
        {
          if (_bitOffset >= _bytes.size() * 8U)
          {
            return std::nullopt;
          }

          auto const byteIndex = _bitOffset / 8U;
          auto const bitIndex = 7U - (_bitOffset % 8U);
          auto const bit = (static_cast<std::uint8_t>(_bytes[byteIndex]) >> bitIndex) & 1U;
          value = (value << 1U) | bit;
          ++_bitOffset;
        }

        return value;
      }

    private:
      std::span<std::byte const> _bytes;
      std::size_t _bitOffset = 0;
    };

    std::optional<std::uint32_t> readAacObjectType(BitReader& reader) noexcept
    {
      auto const optObjectType = reader.read(5);

      if (!optObjectType)
      {
        return std::nullopt;
      }

      if (*optObjectType != kAacEscapeObjectType)
      {
        return optObjectType;
      }

      auto const optExtension = reader.read(6);

      if (!optExtension)
      {
        return std::nullopt;
      }

      return 32U + *optExtension;
    }
  } // namespace

  AacStreamConfig parseAudioSpecificConfig(std::span<std::byte const> bytes) noexcept
  {
    auto reader = BitReader{bytes};

    if (!readAacObjectType(reader))
    {
      return {};
    }

    auto const optSampleRateIndex = reader.read(4);

    if (!optSampleRateIndex)
    {
      return {};
    }

    auto config = AacStreamConfig{};

    if (*optSampleRateIndex == kAacEscapeSampleRateIndex)
    {
      if (auto const optExplicitRate = reader.read(24); optExplicitRate)
      {
        config.sampleRate = *optExplicitRate;
      }
    }
    else if (*optSampleRateIndex < kAacSampleRates.size())
    {
      config.sampleRate = kAacSampleRates.at(*optSampleRateIndex);
    }

    if (auto const optChannelConfig = reader.read(4); optChannelConfig && *optChannelConfig < kAacChannelCounts.size())
    {
      config.channels = kAacChannelCounts.at(*optChannelConfig);
    }

    return config;
  }
} // namespace ao::audio::detail
