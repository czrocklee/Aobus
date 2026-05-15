// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "Frame.h"
#include "FrameLayout.h"

#include <array>
#include <boost/endian/conversion.hpp>  // NOLINT(misc-include-cleaner)
#include <cstdint>
#include <cstring>
#include <optional>

namespace ao::tag::mpeg
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

    constexpr VersionSamplingRateArray kVersionSamplingRateTable = {{
      {kSamplingRate11025, kSamplingRate12000, kSamplingRate8000, 0},  // V2.5 (00)
      {0, 0, 0, 0},                                                    // Reserved (01)
      {kSamplingRate22050, kSamplingRate24000, kSamplingRate16000, 0}, // V2 (10)
      {kSamplingRate44100, kSamplingRate48000, kSamplingRate32000, 0}  // V1 (11)
    }};

    // Number of possible bitrate index values in MPEG audio
    constexpr std::size_t kBitrateCount = 16;
    using BitrateArray = std::array<std::uint16_t, kBitrateCount>;

    constexpr BitrateArray kBitrateTableV1L1 =
      {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0};
    constexpr BitrateArray kBitrateTableV1L2 = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0};
    constexpr BitrateArray kBitrateTableV1L3 = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    constexpr BitrateArray kBitrateTableV2L1 = {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0};
    constexpr BitrateArray kBitrateTableV2L23 = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    constexpr BitrateArray kBitrateTableReserved = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    using LayerBitrateArray = std::array<BitrateArray, 4>;
    using VersionLayerBitrateArray = std::array<LayerBitrateArray, 4>;

    constexpr VersionLayerBitrateArray kVersionLayerBitrateTable = {{
      {kBitrateTableReserved, kBitrateTableV2L23, kBitrateTableV2L23, kBitrateTableV2L1},           // V2.5
      {kBitrateTableReserved, kBitrateTableReserved, kBitrateTableReserved, kBitrateTableReserved}, // Reserved
      {kBitrateTableReserved, kBitrateTableV2L23, kBitrateTableV2L23, kBitrateTableV2L1},           // V2
      {kBitrateTableReserved, kBitrateTableV1L3, kBitrateTableV1L2, kBitrateTableV1L1}              // V1
    }};

    constexpr std::uint8_t kFrameSyncByte1 = 0xFF;
    constexpr std::uint8_t kFrameSyncByte2Mask = 0xE0;
    constexpr std::size_t kXingDataFieldOffset = 8;

    std::uint8_t const* findFrameSync(std::uint8_t const* begin, std::uint8_t const* end)
    {
      while (static_cast<std::size_t>(end - begin) >= sizeof(FrameLayout))
      {
        auto const size = static_cast<std::size_t>(end - begin);
        auto const* sync = static_cast<std::uint8_t const*>(std::memchr(begin, kFrameSyncByte1, size));

        if (sync == nullptr)
        {
          return nullptr;
        }

        if (static_cast<std::size_t>(end - sync) < sizeof(FrameLayout))
        {
          return nullptr;
        }

        if ((*(sync + 1) & kFrameSyncByte2Mask) == kFrameSyncByte2Mask)
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

    auto const bitrate = kVersionLayerBitrateTable.at(versionId).at(layer).at(bitrateIndex);
    auto const samplingRate = kVersionSamplingRateTable.at(versionId).at(samplingRateIndex);

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
      auto const baseLength = (48 * bitrate * kMsPerSecond) / samplingRate;
      return baseLength + (fl.paddingBit() != 0 ? kLayerIPadding : 0);
    }

    // Layer II/III: frame length = (samplesPerFrame / 8 * bitrate) / samplingRate + padding
    constexpr std::uint32_t kMsPerSecond = 1000;
    constexpr std::size_t kLayerIIIIPadding = 1;
    auto const baseLength = (samplesPerFrame() / 8 * bitrate * kMsPerSecond) / samplingRate;
    return baseLength + (fl.paddingBit() != 0 ? kLayerIIIIPadding : 0);
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
    return kVersionSamplingRateTable.at(static_cast<std::size_t>(fl.versionId()))
      .at(static_cast<std::size_t>(fl.samplingRateIndex()));
  }

  std::uint32_t FrameView::bitrate() const
  {
    auto const& fl = layout();
    // Table values are in kbps, convert to bps
    constexpr std::uint32_t kBpsPerKbps = 1000;
    return kVersionLayerBitrateTable.at(static_cast<std::size_t>(fl.versionId()))
             .at(static_cast<std::size_t>(fl.layer()))
             .at(static_cast<std::size_t>(fl.bitrateIndex())) *
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
    static constexpr std::size_t kXingHeaderSize = 4;
    static constexpr std::size_t kXingOffsetVer1Stereo = 32;
    static constexpr std::size_t kXingOffsetVer1Mono = 17;
    static constexpr std::size_t kXingOffsetVer2Stereo = 17;
    static constexpr std::size_t kXingOffsetVer2Mono = 9;

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

    static constexpr std::size_t kXingMagicSize = 4;
    
    if (std::memcmp(ptr, "Xing", kXingMagicSize) != 0 && std::memcmp(ptr, "Info", kXingMagicSize) != 0)
    {
      return {};
    }

    auto info = XingInfo{};
    std::uint32_t flags = 0;
    static constexpr std::size_t kXingFlagsSize = 4;
    std::memcpy(&flags, ptr + kXingFlagsSize, kXingFlagsSize);
    flags = boost::endian::endian_reverse(flags);  // NOLINT(misc-include-cleaner)

    std::size_t fieldOffset = kXingDataFieldOffset;
    constexpr std::uint32_t kXingFlagFrames = 0x01;
    constexpr std::uint32_t kXingFlagBytes = 0x02;

    if ((flags & kXingFlagFrames) != 0)
    {
      std::uint32_t frames = 0;
      static constexpr std::size_t kXingFramesFieldSize = 4;
      std::memcpy(&frames, ptr + fieldOffset, kXingFramesFieldSize);
      info.frames = boost::endian::endian_reverse(frames);  // NOLINT(misc-include-cleaner)
      fieldOffset += kXingFramesFieldSize;
    }

    if ((flags & kXingFlagBytes) != 0) // Bytes field present
    {
      std::uint32_t bytes = 0;
      static constexpr std::size_t kXingBytesFieldSize = 4;
      std::memcpy(&bytes, ptr + fieldOffset, kXingBytesFieldSize);
      info.bytes = boost::endian::endian_reverse(bytes);  // NOLINT(misc-include-cleaner)
      fieldOffset += kXingBytesFieldSize;
    }

    return info;
  }
} // namespace ao::tag::mpeg
