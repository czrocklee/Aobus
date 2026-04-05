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

    constexpr VersionSamplingRateArray VersionSamplingRateTable = {{
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

    constexpr VersionLayerBitrateArray VersionLayerBitrateTable = {{
      {BitrateTableReserved, BitrateTableV2L23, BitrateTableV2L23, BitrateTableV2L1},           // V2.5
      {BitrateTableReserved, BitrateTableReserved, BitrateTableReserved, BitrateTableReserved}, // Reserved
      {BitrateTableReserved, BitrateTableV2L23, BitrateTableV2L23, BitrateTableV2L1},           // V2
      {BitrateTableReserved, BitrateTableV1L3, BitrateTableV1L2, BitrateTableV1L1}            // V1
    }};

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

  bool FrameView::isValid() const
  {
    auto const& header = layout();

    // sync1 must be 0xFF
    if (header.sync1 != 0xFF) { return false; }

    // sync2 bits (top 3 bits of second byte) must be 0b111
    // This is checked by: (byte & 0xE0) == 0xE0
    auto const secondByte = reinterpret_cast<unsigned char const*>(&header)[1];
    if ((secondByte & 0xE0) != 0xE0) { return false; }

    // versionId cannot be Reserved (0b01)
    if (header.versionId == VersionID::Reserved) { return false; }

    // layer cannot be Reserved (0b00)
    if (header.layer == LayerDescription::Reserved) { return false; }

    // bitrateIndex cannot be 0 (free) or 15 (reserved)
    if (header.bitrateIndex == 0 || header.bitrateIndex == 15) { return false; }

    // samplingRateIndex cannot be 3 (reserved)
    if (header.samplingRateIndex == 3) { return false; }

    return true;
  }

  std::size_t FrameView::length() const
  {
    auto const& fl = layout();
    auto const versionId = static_cast<std::size_t>(fl.versionId);
    auto const layer = static_cast<std::size_t>(fl.layer);
    auto const bitrateIndex = static_cast<std::size_t>(fl.bitrateIndex);
    auto const samplingRateIndex = static_cast<std::size_t>(fl.samplingRateIndex);

    auto const bitrate = VersionLayerBitrateTable[versionId][layer][bitrateIndex];
    auto const samplingRate = VersionSamplingRateTable[versionId][samplingRateIndex];

    if (bitrate == 0 || samplingRate == 0) { return 0; }

    if (fl.layer == LayerDescription::LayerI)
    {
      // Layer I: frame length = (384 * bitrate) / samplingRate + padding
      return (384 * bitrate * 1000) / samplingRate + (fl.paddingBit ? 4 : 0);
    }
    else
    {
      // Layer II/III: frame length = (144 * bitrate) / samplingRate + padding
      return (144 * bitrate * 1000) / samplingRate + (fl.paddingBit ? 1 : 0);
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

  std::uint32_t FrameView::sampleRate() const
  {
    auto const& fl = layout();
    return VersionSamplingRateTable[static_cast<std::size_t>(fl.versionId)]
                                  [static_cast<std::size_t>(fl.samplingRateIndex)];
  }

  std::uint32_t FrameView::bitrate() const
  {
    auto const& fl = layout();
    // Table values are in kbps, convert to bps
    return VersionLayerBitrateTable[static_cast<std::size_t>(fl.versionId)]
                                  [static_cast<std::size_t>(fl.layer)]
                                  [static_cast<std::size_t>(fl.bitrateIndex)] * 1000;
  }

  std::uint8_t FrameView::channels() const
  {
    auto const& fl = layout();
    // SingleChannel = 1, all others = 2
    return fl.channelMode == ChannelMode::SingleChannel ? 1 : 2;
  }

} // namespace rs::tag::mpeg
