/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "Frame.h"
#include <cstring>
#include <array>

namespace rs::tag::mpeg
{
  namespace
  {
    using SamplingRateArray = std::array<std::uint32_t, 4>;
    using VersionSamplingRateArray = std::array<SamplingRateArray, 4>;

    VersionSamplingRateArray VersionSamplingRateTable = {{
      {44100, 48000, 32000, 0}, // V2.5
      {0, 0, 0, 0},             // Reserved
      {22050, 24000, 16000, 0}, // V2
      {11025, 12000, 8000, 0}   // V1
    }};

    using BitrateArray = std::array<std::uint16_t, 16>;

    constexpr BitrateArray BitrateTableV1L1 = {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 353, 384, 416, 448, 0};
    constexpr BitrateArray BitrateTableV1L2 = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0};
    constexpr BitrateArray BitrateTableV1L3 = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    constexpr BitrateArray BitrateTableV2L1 = {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0};
    constexpr BitrateArray BitrateTableV2L23 = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    constexpr BitrateArray BitrateTableReserved = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    using LayerBitrateArray = std::array<BitrateArray, 4>;
    using VersionLayerBitrateArray = std::array<LayerBitrateArray, 4>;

    VersionLayerBitrateArray VersionLayerBitrateTable = {{
      {BitrateTableReserved, BitrateTableV2L23, BitrateTableV2L23, BitrateTableV2L1},           // V2.5
      {BitrateTableReserved, BitrateTableReserved, BitrateTableReserved, BitrateTableReserved}, // Reserved,
      {BitrateTableReserved, BitrateTableV2L23, BitrateTableV2L23, BitrateTableV2L1},           // V2
      {BitrateTableReserved, BitrateTableV1L3, BitrateTableV1L2, BitrateTableV1L1}              // V1
    }};
  }

  std::size_t FrameView::length() const
  {
    const auto& frameLayout = layout();
    std::size_t versionId = static_cast<std::size_t>(frameLayout.versionId);
    std::size_t layer = static_cast<std::size_t>(frameLayout.layer);
    std::size_t bitrateIndex = static_cast<std::size_t>(frameLayout.bitrateIndex);
    std::size_t samplingRateIndex = static_cast<std::size_t>(frameLayout.samplingRateIndex);

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

  bool FrameView::isValid() const {}

  namespace
  {
    constexpr std::uint8_t FrameSyncByte1 = 0xFF;
    constexpr std::uint8_t FrameSyncByte2Mask = 0xE0;
    constexpr std::size_t FrameSyncSkipSize = 2;

    const std::uint8_t* findFrameSync(const std::uint8_t* begin, const std::uint8_t* end)
    {
      for (auto size = static_cast<std::size_t>(end - begin); size >= sizeof(FrameLayout);)
      {
        if (begin = static_cast<const std::uint8_t*>(std::memchr(begin, FrameSyncByte1, size)); begin == nullptr)
        {
          return nullptr;
        }

        if (size = static_cast<std::size_t>(end - begin); size < sizeof(FrameLayout))
        {
          return nullptr;
        }
        else if ((*(begin + 1) & FrameSyncByte2Mask) == FrameSyncByte2Mask)
        {
          return begin;
        }
        else
        {
          begin += FrameSyncSkipSize;
        }
      }

      return nullptr;
    }
  }

  std::optional<FrameView> locate(const void* buffer, std::size_t size)
  {
    auto begin = static_cast<const std::uint8_t*>(buffer);
    auto end = begin + size;

    for (const std::uint8_t* frameCandidate = findFrameSync(begin, end); frameCandidate != nullptr;
         frameCandidate = findFrameSync(frameCandidate + FrameSyncSkipSize, end))
    {
      if (FrameView view{frameCandidate, static_cast<std::size_t>(end - frameCandidate)}; view.isValid())
      {
        return view;
      }
    }

    return {};
  }
}
