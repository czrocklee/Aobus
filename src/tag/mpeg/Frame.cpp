// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "Frame.h"
#include <array>
#include <cstdint>
#include <cstring>

namespace rs::tag::mpeg
{
  namespace
  {
    // Sampling rates in Hz
    constexpr std::uint32_t kSamplingRateV25 = 44100;
    constexpr std::uint32_t kSamplingRateV2 = 22050;
    constexpr std::uint32_t kSamplingRateV1 = 11025;
    constexpr std::uint32_t kSamplingRate48000 = 48000;
    constexpr std::uint32_t kSamplingRate24000 = 24000;
    constexpr std::uint32_t kSamplingRate12000 = 12000;
    constexpr std::uint32_t kSamplingRate32000 = 32000;
    constexpr std::uint32_t kSamplingRate16000 = 16000;
    constexpr std::uint32_t kSamplingRate8000 = 8000;

    using SamplingRateArray = std::array<std::uint32_t, 4>;
    using VersionSamplingRateArray = std::array<SamplingRateArray, 4>;

    [[maybe_unused]] constexpr VersionSamplingRateArray VersionSamplingRateTable = {{
      {kSamplingRateV25, kSamplingRate48000, kSamplingRate32000, 0}, // V2.5
      {0, 0, 0, 0},             // Reserved
      {kSamplingRateV2, kSamplingRate24000, kSamplingRate16000, 0}, // V2
      {kSamplingRateV1, kSamplingRate12000, kSamplingRate8000, 0}   // V1
    }};

    // Number of possible bitrate index values in MPEG audio
    constexpr std::size_t kBitrateCount = 16;
    using BitrateArray = std::array<std::uint16_t, kBitrateCount>;

    constexpr BitrateArray BitrateTableV1L1 = {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 353, 384, 416, 448, 0};
    constexpr BitrateArray BitrateTableV1L2 = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0};
    constexpr BitrateArray BitrateTableV1L3 = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    constexpr BitrateArray BitrateTableV2L1 = {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0};
    constexpr BitrateArray BitrateTableV2L23 = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    constexpr BitrateArray BitrateTableReserved = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    using LayerBitrateArray = std::array<BitrateArray, 4>;
    using VersionLayerBitrateArray = std::array<LayerBitrateArray, 4>;

    [[maybe_unused]] constexpr VersionLayerBitrateArray VersionLayerBitrateTable = {{
      {BitrateTableReserved, BitrateTableV2L23, BitrateTableV2L23, BitrateTableV2L1},           // V2.5
      {BitrateTableReserved, BitrateTableReserved, BitrateTableReserved, BitrateTableReserved}, // Reserved,
      {BitrateTableReserved, BitrateTableV2L23, BitrateTableV2L23, BitrateTableV2L1},           // V2
      {BitrateTableReserved, BitrateTableV1L3, BitrateTableV1L2, BitrateTableV1L1}              // V1
    }};
  }

  std::size_t FrameView::length() const
  {
    auto const& frameLayout = layout();
    [[maybe_unused]] std::size_t versionId = static_cast<std::size_t>(frameLayout.versionId);
    [[maybe_unused]] std::size_t layer = static_cast<std::size_t>(frameLayout.layer);
    [[maybe_unused]] std::size_t bitrateIndex = static_cast<std::size_t>(frameLayout.bitrateIndex);
    [[maybe_unused]] std::size_t samplingRateIndex = static_cast<std::size_t>(frameLayout.samplingRateIndex);
    return 0;
    /* if (frameLayout.layer == LayerDescription::LayerI)
    {
        384 * bitrate * 125 / samplingRate

      return (12000 * VersionLayerBitrateTable[versionId][layer][bitrateIndex] /
                VersionSamplingRateTable[versionId][samplingRateIndex] +
              frameLayout.paddingBit) *
             4;
    }
    else
    {
      return
    } */
  }

  bool FrameView::isValid() const { return false; }

  namespace
  {
    constexpr std::uint8_t FrameSyncByte1 = 0xFF;
    constexpr std::uint8_t FrameSyncByte2Mask = 0xE0;
    constexpr std::size_t FrameSyncSkipSize = 2;

    std::uint8_t const* findFrameSync(std::uint8_t const* begin, std::uint8_t const* end)
    {
        for (auto size = static_cast<std::size_t>(end - begin); size >= sizeof(FrameLayout);)
      {
        if (begin = static_cast<std::uint8_t const*>(std::memchr(begin, FrameSyncByte1, size)); begin == nullptr)
        {
          return nullptr;
        }

        if (size = static_cast<std::size_t>(end - begin); size < sizeof(FrameLayout))
        {
          return nullptr;
        }
        if ((*(begin + 1) & FrameSyncByte2Mask) == FrameSyncByte2Mask)
        {
          return begin;
        }
        begin += FrameSyncSkipSize;
      }

      return nullptr;
    }
  }

  std::optional<FrameView> locate(void const* buffer, std::size_t size)
  {
    std::uint8_t const* const begin = static_cast<std::uint8_t const*>(buffer);
    std::uint8_t const* const end = begin + size;

    for (std::uint8_t const* frameCandidate = findFrameSync(begin, end); frameCandidate != nullptr;
         frameCandidate = findFrameSync(frameCandidate + FrameSyncSkipSize, end))
    {
      if (auto view = FrameView{frameCandidate, static_cast<std::size_t>(end - frameCandidate)}; view.isValid())
      {
        return view;
      }
    }

    return {};
  }
}
