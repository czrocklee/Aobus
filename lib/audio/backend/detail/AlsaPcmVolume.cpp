// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/backend/detail/AlsaPcmVolume.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

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
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — ALSA PCM byte buffer to typed view
      auto* samples = reinterpret_cast<std::int16_t*>(pcm.data());
      std::size_t const count = pcm.size() / sizeof(std::int16_t);

      for (std::size_t i = 0; i < count; ++i)
      {
        samples[i] = scaleSample(samples[i], gain);
      }
    }

    void applyS32(std::span<std::byte> pcm, float gain, std::uint8_t validBits) noexcept
    {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — ALSA PCM byte buffer to typed view
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — ALSA PCM byte buffer to typed view
      auto* samples = reinterpret_cast<std::int32_t*>(
        pcm.data()); // NOLINT(aobus-readability-use-if-init-statement) — used in both branches
      // NOLINTNEXTLINE(aobus-readability-use-if-init-statement) — used in both branches
      std::size_t const count = pcm.size() / sizeof(std::int32_t);

      if (validBits == 24)
      {
        for (std::size_t i = 0; i < count; ++i)
        {
          // NOLINTNEXTLINE(bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions) — intentional int64
          // from double round
          std::int64_t const scaled =
            static_cast<std::int64_t>(std::round(static_cast<double>(samples[i]) * static_cast<double>(gain)));
          samples[i] = static_cast<std::int32_t>(std::clamp<std::int64_t>(scaled, kS24Min, kS24Max));
        }
      }
      else
      {
        for (std::size_t i = 0; i < count; ++i)
        {
          // NOLINTNEXTLINE(bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions) — intentional int64
          // from double round
          std::int64_t const scaled =
            static_cast<std::int64_t>(std::round(static_cast<double>(samples[i]) * static_cast<double>(gain)));
          samples[i] = static_cast<std::int32_t>(std::clamp<std::int64_t>(scaled, INT32_MIN, INT32_MAX));
        }
      }
    }

    void applyS243Le(std::span<std::byte> pcm, float gain) noexcept
    {
      constexpr std::size_t kS243LeBytes = 3;
      constexpr std::uint32_t kSignExtendMask = 0xFF000000;
      constexpr std::uint32_t kSignBit24 = 0x800000;
      constexpr std::uint32_t kByteMask = 0xFF;
      constexpr std::int32_t kShift8 = 8;
      constexpr std::int32_t kShift16 = 16;

      for (std::size_t i = 0; i + 2 < pcm.size(); i += kS243LeBytes)
      {
        // Unpack 24-bit little-endian signed integer
        std::uint32_t const b0 = static_cast<std::uint8_t>(pcm[i]);
        std::uint32_t const b1 = static_cast<std::uint8_t>(pcm[i + 1]);
        std::uint32_t const b2 = static_cast<std::uint8_t>(pcm[i + 2]);
        std::uint32_t uSample = b0 | (b1 << kShift8) | (b2 << kShift16);

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
        pcm[i + 1] = static_cast<std::byte>((finalSample >> kShift8) & kByteMask);
        pcm[i + 2] = static_cast<std::byte>((finalSample >> kShift16) & kByteMask);
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
