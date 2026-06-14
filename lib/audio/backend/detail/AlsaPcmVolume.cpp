// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/backend/detail/AlsaPcmVolume.h>
#include <ao/utility/ByteView.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace ao::audio::backend::detail
{
  namespace
  {
    constexpr float kVolumeEpsilon = 1e-4F;
    constexpr std::int32_t kS24Min = -8388608;
    constexpr std::int32_t kS24Max = 8388607;

    template<typename T>
    T scaleSample(T sample, float gain) noexcept
    {
      return static_cast<T>(std::round(static_cast<float>(sample) * gain));
    }

    void applyS16(std::span<std::byte> pcm, float gain) noexcept
    {
      auto const samples = utility::layout::viewArrayMutable<std::int16_t>(pcm);

      for (auto& sample : samples)
      {
        sample = scaleSample(sample, gain);
      }
    }

    void applyS32(std::span<std::byte> pcm, float gain, std::uint8_t validBits) noexcept
    {
      auto const samples = utility::layout::viewArrayMutable<std::int32_t>(pcm);
      auto const [limitMin, limitMax] = [validBits] -> std::pair<std::int64_t, std::int64_t>
      {
        if (validBits == 24)
        {
          return {kS24Min, kS24Max};
        }

        return {std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()};
      }();

      for (auto& sample : samples)
      {
        std::int64_t const scaled =
          static_cast<std::int64_t>(std::round(static_cast<double>(sample) * static_cast<double>(gain)));
        sample = static_cast<std::int32_t>(std::clamp<std::int64_t>(scaled, limitMin, limitMax));
      }
    }

    void applyS243Le(std::span<std::byte> pcm, float gain) noexcept
    {
      constexpr std::size_t kS243LeBytes = 3;
      constexpr std::uint32_t kSignExtendMask = 0xFF000000;
      constexpr std::uint32_t kSignBit24 = 0x800000;
      constexpr std::uint32_t kByteMask = 0xFF;

      for (std::size_t i = 0; i + 2 < pcm.size(); i += kS243LeBytes)
      {
        // Unpack 24-bit little-endian signed integer
        std::uint32_t const b0 = static_cast<std::uint8_t>(pcm[i]);
        std::uint32_t const b1 = static_cast<std::uint8_t>(pcm[i + 1]);
        std::uint32_t const b2 = static_cast<std::uint8_t>(pcm[i + 2]);
        std::uint32_t uSample = b0 | (b1 << 8) | (b2 << 16);

        // Sign extend from 24 to 32 bits
        if ((uSample & kSignBit24) != 0)
        {
          uSample |= kSignExtendMask;
        }

        auto sample = static_cast<std::int32_t>(uSample);
        // NOLINTNEXTLINE(bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions) — intentional int64
        // from double round
        std::int64_t const scaled =
          static_cast<std::int64_t>(std::round(static_cast<double>(sample) * static_cast<double>(gain)));
        std::int32_t const finalSample = static_cast<std::int32_t>(std::clamp<std::int64_t>(scaled, kS24Min, kS24Max));

        // Repack 3 bytes
        pcm[i] = static_cast<std::byte>(finalSample & kByteMask);
        pcm[i + 1] = static_cast<std::byte>((finalSample >> 8) & kByteMask);
        pcm[i + 2] = static_cast<std::byte>((finalSample >> 16) & kByteMask);
      }
    }
  } // namespace

  void applyAlsaSoftwareGain(std::span<std::byte> pcm,
                             std::uint8_t bitDepth,
                             std::uint8_t validBits,
                             bool is3Byte24Bit,
                             float gain) noexcept
  {
    if (gain > 1.0F - kVolumeEpsilon)
    {
      return;
    }

    if (gain < kVolumeEpsilon)
    {
      std::ranges::fill(pcm, std::byte{0});
      return;
    }

    if (is3Byte24Bit)
    {
      applyS243Le(pcm, gain);
      return;
    }

    if (bitDepth == 16)
    {
      applyS16(pcm, gain);
    }
    else if (bitDepth == 32)
    {
      applyS32(pcm, gain, validBits);
    }
  }
} // namespace ao::audio::backend::detail
