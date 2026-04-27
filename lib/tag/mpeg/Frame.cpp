// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "Frame.h"
#include <array>
#include <boost/endian/conversion.hpp>
#include <cstdint>
#include <cstring>

namespace rs::tag::mpeg
{
  namespace
  {
    // Sampling rates in Hz
    constexpr std::uint32_t kSamplingRate44100 = 44100;
    constexpr std::uint32_t kSamplingRate48000 = 48000;
    constexpr std::uint32_t kSamplingRate32000 = 32000;
    constexpr std::uint32_t kSamplingRate22050 = 22050;
    constexpr std::uint32_t kSamplingRate24000 = 24000;
    constexpr std::uint32_t kSamplingRate16000 = 16000;
    constexpr std::uint32_t kSamplingRate11025 = 11025;
    constexpr std::uint32_t kSamplingRate12000 = 12000;
    constexpr std::uint32_t kSamplingRate8000 = 8000;

    using SamplingRateArray = std::array<std::uint32_t, 4>;
    using VersionSamplingRateArray = std::array<SamplingRateArray, 4>;

    constexpr VersionSamplingRateArray VersionSamplingRateTable = {{
      {kSamplingRate11025, kSamplingRate12000, kSamplingRate8000, 0},  // V2.5 (00)
      {0, 0, 0, 0},                                                    // Reserved (01)
      {kSamplingRate22050, kSamplingRate24000, kSamplingRate16000, 0}, // V2 (10)
      {kSamplingRate44100, kSamplingRate48000, kSamplingRate32000, 0}  // V1 (11)
    }};

    // Number of possible bitrate index values in MPEG audio
    constexpr std::size_t kBitrateCount = 16;
    using BitrateArray = std::array<std::uint16_t, kBitrateCount>;

    constexpr BitrateArray BitrateTableV1L1 = {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0};
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
      {BitrateTableReserved, BitrateTableV1L3, BitrateTableV1L2, BitrateTableV1L1}              // V1
    }};

    constexpr std::uint8_t FrameSyncByte1 = 0xFF;
    constexpr std::uint8_t FrameSyncByte2Mask = 0xE0;
    constexpr std::size_t FrameSyncSkipSize = 2;

    std::uint8_t const* findFrameSync(std::uint8_t const* begin, std::uint8_t const* end)
    {
      while (static_cast<std::size_t>(end - begin) >= sizeof(FrameLayout))
      {
        auto const size = static_cast<std::size_t>(end - begin);
        auto const* sync = static_cast<std::uint8_t const*>(std::memchr(begin, FrameSyncByte1, size));

        if (sync == nullptr)
        {
          return nullptr;
        }

        if (static_cast<std::size_t>(end - sync) < sizeof(FrameLayout))
        {
          return nullptr;
        }

        if ((*(sync + 1) & FrameSyncByte2Mask) == FrameSyncByte2Mask)
        {
          return sync;
        }

        begin = sync + 1;
      }

      return nullptr;
    }
  }

  bool FrameView::isValid() const
  {
    constexpr std::uint8_t kFrameSyncByte = 0xFF;
    constexpr std::uint8_t kSync2Expected = 0x07;
    constexpr std::uint8_t kBitrateIndexFree = 0;
    constexpr std::uint8_t kBitrateIndexReserved = 15;
    constexpr std::uint8_t kSamplingRateIndexReserved = 3;

    auto const& header = layout();

    // sync1 must be 0xFF

    if (header.sync1() != kFrameSyncByte)
    {
      return false;
    }

    // sync2 bits (top 3 bits of second byte) must be 0b111

    if (header.sync2() != kSync2Expected)
    {
      return false;
    }

    // versionId cannot be Reserved (0b01)

    if (header.versionId() == VersionID::Reserved)
    {
      return false;
    }

    // layer cannot be Reserved (0b00)

    if (header.layer() == LayerDescription::Reserved)
    {
      return false;
    }

    // bitrateIndex cannot be 0 (free) or 15 (reserved)

    if (header.bitrateIndex() == kBitrateIndexFree || header.bitrateIndex() == kBitrateIndexReserved)
    {
      return false;
    }

    // samplingRateIndex cannot be 3 (reserved)

    if (header.samplingRateIndex() == kSamplingRateIndexReserved)
    {
      return false;
    }

    return true;
  }

  std::size_t FrameView::length() const
  {
    auto const& fl = layout();
    auto const versionId = static_cast<std::size_t>(fl.versionId());
    auto const layer = static_cast<std::size_t>(fl.layer());
    auto const bitrateIndex = static_cast<std::size_t>(fl.bitrateIndex());
    auto const samplingRateIndex = static_cast<std::size_t>(fl.samplingRateIndex());

    auto const bitrate = VersionLayerBitrateTable[versionId][layer][bitrateIndex];
    auto const samplingRate = VersionSamplingRateTable[versionId][samplingRateIndex];

    if (bitrate == 0 || samplingRate == 0)
    {
      return 0;
    }

    if (fl.layer() == LayerDescription::LayerI)
    {
      // Layer I: frame length = (384 * bitrate) / samplingRate + padding
      // 384 / 8 = 48
      constexpr std::uint32_t kMsPerSecond = 1000;
      constexpr std::size_t kLayerIPadding = 4;
      return (48 * bitrate * kMsPerSecond) / samplingRate + (fl.paddingBit() ? kLayerIPadding : 0);
    }

    // Layer II/III: frame length = (samplesPerFrame / 8 * bitrate) / samplingRate + padding
    constexpr std::uint32_t kMsPerSecond = 1000;
    constexpr std::size_t kLayerIIIIPadding = 1;
    return (samplesPerFrame() / 8 * bitrate * kMsPerSecond) / samplingRate + (fl.paddingBit() ? kLayerIIIIPadding : 0);
  }

  std::optional<FrameView> locate(void const* buffer, std::size_t size)
  {
    std::uint8_t const* const begin = static_cast<std::uint8_t const*>(buffer);
    std::uint8_t const* const end = begin + size;

    for (std::uint8_t const* frameCandidate = findFrameSync(begin, end); frameCandidate != nullptr;
         frameCandidate = findFrameSync(frameCandidate + 1, end))
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
    return VersionSamplingRateTable[static_cast<std::size_t>(fl.versionId())]
                                   [static_cast<std::size_t>(fl.samplingRateIndex())];
  }

  std::uint32_t FrameView::bitrate() const
  {
    auto const& fl = layout();
    // Table values are in kbps, convert to bps
    constexpr std::uint32_t kBpsPerKbps = 1000;
    return VersionLayerBitrateTable[static_cast<std::size_t>(fl.versionId())][static_cast<std::size_t>(fl.layer())]
                                   [static_cast<std::size_t>(fl.bitrateIndex())] *
           kBpsPerKbps;
  }

  std::uint8_t FrameView::channels() const
  {
    auto const& fl = layout();
    // SingleChannel = 1, all others = 2
    return fl.channelMode() == ChannelMode::SingleChannel ? 1 : 2;
  }

  std::uint16_t FrameView::samplesPerFrame() const
  {
    auto const& fl = layout();
    auto const layer = fl.layer();
    constexpr std::uint16_t kSamplesLayerI = 384;
    constexpr std::uint16_t kSamplesLayerII = 1152;
    constexpr std::uint16_t kSamplesLayerIIIVer1 = 1152;
    constexpr std::uint16_t kSamplesLayerIIIVer2 = 576;

    if (layer == LayerDescription::LayerI)
    {
      return kSamplesLayerI;
    }

    if (layer == LayerDescription::LayerII)
    {
      return kSamplesLayerII;
    }

    if (layer == LayerDescription::LayerIII)
    {
      return (fl.versionId() == VersionID::Ver1) ? kSamplesLayerIIIVer1 : kSamplesLayerIIIVer2;
    }

    return 0;
  }

  std::optional<FrameView::XingInfo> FrameView::xingInfo() const
  {
    auto const& fl = layout();

    if (fl.layer() != LayerDescription::LayerIII)
    {
      return {};
    }

    // Offset of "Xing" or "Info" relative to frame start (excluding 4-byte header)
    std::size_t offset = 0;
    constexpr std::size_t kXingHeaderSize = 4;
    constexpr std::size_t kXingOffsetVer1Stereo = 32;
    constexpr std::size_t kXingOffsetVer1Mono = 17;
    constexpr std::size_t kXingOffsetVer2Stereo = 17;
    constexpr std::size_t kXingOffsetVer2Mono = 9;

    if (fl.versionId() == VersionID::Ver1)
    {
      offset = (fl.channelMode() == ChannelMode::SingleChannel) ? kXingOffsetVer1Mono : kXingOffsetVer1Stereo;
    }
    else
    {
      offset = (fl.channelMode() == ChannelMode::SingleChannel) ? kXingOffsetVer2Mono : kXingOffsetVer2Stereo;
    }

    // Header is 4 bytes
    offset += kXingHeaderSize;

    auto const* ptr = static_cast<std::uint8_t const*>(_data) + offset;

    if (std::memcmp(ptr, "Xing", 4) != 0 && std::memcmp(ptr, "Info", 4) != 0)
    {
      return {};
    }

    XingInfo info;
    std::uint32_t flags = 0;
    std::memcpy(&flags, ptr + 4, 4);
    flags = boost::endian::endian_reverse(flags);

    std::size_t fieldOffset = 8;
    constexpr std::uint32_t kXingFlagFrames = 0x01;
    constexpr std::uint32_t kXingFlagBytes = 0x02;

    if (flags & kXingFlagFrames)
    {
      std::uint32_t frames = 0;
      std::memcpy(&frames, ptr + fieldOffset, 4);
      info.frames = boost::endian::endian_reverse(frames);
      fieldOffset += 4;
    }

    if (flags & kXingFlagBytes) // Bytes field present
    {
      std::uint32_t bytes = 0;
      std::memcpy(&bytes, ptr + fieldOffset, 4);
      info.bytes = boost::endian::endian_reverse(bytes);
      fieldOffset += 4;
    }

    return info;
  }

} // namespace rs::tag::mpeg
