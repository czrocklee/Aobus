// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "Frame.h"

#include "FrameLayout.h"
#include <ao/utility/ByteView.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>

namespace ao::media::file::mpeg
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

    using SamplingRateChoices = std::array<std::uint32_t, 4>;
    using VersionSamplingRates = std::array<SamplingRateChoices, 4>;

    constexpr VersionSamplingRates kVersionSamplingRateTable = {{
      {kSamplingRate11025, kSamplingRate12000, kSamplingRate8000, 0},  // V2.5 (00)
      {0, 0, 0, 0},                                                    // Reserved (01)
      {kSamplingRate22050, kSamplingRate24000, kSamplingRate16000, 0}, // V2 (10)
      {kSamplingRate44100, kSamplingRate48000, kSamplingRate32000, 0}  // V1 (11)
    }};

    // Number of possible bitrate index values in MPEG audio
    constexpr std::size_t kBitrateCount = 16;
    using BitrateChoices = std::array<std::uint16_t, kBitrateCount>;

    constexpr BitrateChoices kBitrateTableV1L1 =
      {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0};
    constexpr BitrateChoices kBitrateTableV1L2 = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0};
    constexpr BitrateChoices kBitrateTableV1L3 = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    constexpr BitrateChoices kBitrateTableV2L1 = {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0};
    constexpr BitrateChoices kBitrateTableV2L23 = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    constexpr BitrateChoices kBitrateTableReserved = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    using LayerBitrates = std::array<BitrateChoices, 4>;
    using VersionLayerBitrates = std::array<LayerBitrates, 4>;

    constexpr VersionLayerBitrates kVersionLayerBitrateTable = {{
      {kBitrateTableReserved, kBitrateTableV2L23, kBitrateTableV2L23, kBitrateTableV2L1},           // V2.5
      {kBitrateTableReserved, kBitrateTableReserved, kBitrateTableReserved, kBitrateTableReserved}, // Reserved
      {kBitrateTableReserved, kBitrateTableV2L23, kBitrateTableV2L23, kBitrateTableV2L1},           // V2
      {kBitrateTableReserved, kBitrateTableV1L3, kBitrateTableV1L2, kBitrateTableV1L1}              // V1
    }};

    constexpr std::uint8_t kFrameSyncByte1 = 0xFF;
    constexpr std::uint8_t kFrameSyncByte2Mask = 0xE0;
    [[maybe_unused]] constexpr std::size_t kXingDataFieldOffset = 8;

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

    std::size_t paddingBytes(FrameView const& frame) noexcept
    {
      if (frame.layout().paddingBit() == 0)
      {
        return 0;
      }

      return frame.layout().layer() == LayerDescription::LayerI ? 4U : 1U;
    }

    bool hasMatchingStreamParameters(FrameView const& lhs, FrameView const& rhs) noexcept
    {
      auto const& lhsLayout = lhs.layout();
      auto const& rhsLayout = rhs.layout();
      return lhsLayout.versionId() == rhsLayout.versionId() && lhsLayout.layer() == rhsLayout.layer() &&
             lhsLayout.samplingRateIndex() == rhsLayout.samplingRateIndex();
    }

    bool confirmsKnownRateFrame(FrameView const& frame, std::uint8_t const* frameStart, std::uint8_t const* end)
    {
      auto const frameLength = frame.length();
      auto const remaining = static_cast<std::size_t>(end - frameStart);

      if (frameLength < sizeof(FrameLayout) || frameLength > remaining)
      {
        return false;
      }

      auto const* const nextStart = frameStart + frameLength;

      if (static_cast<std::size_t>(end - nextStart) < sizeof(FrameLayout))
      {
        return true;
      }

      auto const next = FrameView{nextStart, static_cast<std::size_t>(end - nextStart)};

      if (!next.isValid() || next.layout().bitrateIndex() == 0 || !hasMatchingStreamParameters(frame, next))
      {
        return false;
      }

      auto const nextLength = next.length();
      return nextLength >= sizeof(FrameLayout) && nextLength <= next.size();
    }

    std::optional<std::size_t> resolveFreeFormatLength(std::uint8_t const* frameStart, std::uint8_t const* end)
    {
      auto const frame = FrameView{frameStart, static_cast<std::size_t>(end - frameStart)};

      for (auto const* nextStart = findFrameSync(frameStart + 1, end); nextStart != nullptr;
           nextStart = findFrameSync(nextStart + 1, end))
      {
        auto const next = FrameView{nextStart, static_cast<std::size_t>(end - nextStart)};

        if (!next.isValid() || next.layout().bitrateIndex() != 0 || !hasMatchingStreamParameters(frame, next))
        {
          continue;
        }

        auto const frameLength = static_cast<std::size_t>(nextStart - frameStart);
        auto const currentPadding = paddingBytes(frame);

        if (frameLength <= currentPadding)
        {
          continue;
        }

        auto const baseLength = frameLength - currentPadding;
        auto const nextLength = baseLength + paddingBytes(next);
        auto const nextRemaining = static_cast<std::size_t>(end - nextStart);

        if (nextLength < sizeof(FrameLayout) || nextLength > nextRemaining)
        {
          continue;
        }

        auto const* const thirdStart = nextStart + nextLength;

        if (thirdStart == end)
        {
          return frameLength;
        }

        if (static_cast<std::size_t>(end - thirdStart) < sizeof(FrameLayout))
        {
          continue;
        }

        auto const third = FrameView{thirdStart, static_cast<std::size_t>(end - thirdStart)};

        if (third.isValid() && third.layout().bitrateIndex() == 0 && hasMatchingStreamParameters(frame, third))
        {
          return frameLength;
        }
      }

      return std::nullopt;
    }
  } // namespace

  bool FrameView::isValid() const
  {
    constexpr std::uint8_t kFrameSyncByte = 0xFF;
    constexpr std::uint8_t kSync2Expected = 0x07;
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
    if (header.versionId() == VersionId::Reserved)
    {
      return false;
    }

    // layer cannot be Reserved (0b00)
    if (header.layer() == LayerDescription::Reserved)
    {
      return false;
    }

    // The all-zero bitrate index is free format and is resolved from adjacent frames.
    if (header.bitrateIndex() == kBitrateIndexReserved)
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
    if (_resolvedLength != 0)
    {
      return _resolvedLength;
    }

    auto const& fl = layout();
    auto const versionId = static_cast<std::size_t>(fl.versionId());
    auto const layer = static_cast<std::size_t>(fl.layer());
    auto const bitrateIndex = static_cast<std::size_t>(fl.bitrateIndex());
    auto const samplingRateIndex = static_cast<std::size_t>(fl.samplingRateIndex());

    auto const bitrate = kVersionLayerBitrateTable[versionId][layer][bitrateIndex];
    auto const samplingRate = kVersionSamplingRateTable[versionId][samplingRateIndex];

    if (bitrate == 0 || samplingRate == 0)
    {
      return 0;
    }

    if (fl.layer() == LayerDescription::LayerI)
    {
      // Layer I: frame length = floor(12 * bitrate / sampleRate + padding) * 4.
      constexpr std::uint32_t kMsPerSecond = 1000;
      constexpr std::size_t kLayerIPadding = 4;
      auto const baseLength = ((12 * bitrate * kMsPerSecond) / samplingRate) * 4;
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
      auto const view = FrameView{frameCandidate, static_cast<std::size_t>(end - frameCandidate)};

      if (!view.isValid())
      {
        continue;
      }

      if (view.layout().bitrateIndex() == 0)
      {
        if (auto const optLength = resolveFreeFormatLength(frameCandidate, end); optLength)
        {
          return FrameView{frameCandidate, view.size(), *optLength};
        }

        continue;
      }

      if (confirmsKnownRateFrame(view, frameCandidate, end))
      {
        return view;
      }
    }

    return {};
  }

  std::uint32_t FrameView::sampleRate() const
  {
    auto const& fl = layout();
    return kVersionSamplingRateTable[static_cast<std::size_t>(fl.versionId())]
                                    [static_cast<std::size_t>(fl.samplingRateIndex())];
  }

  std::uint32_t FrameView::bitrate() const
  {
    auto const& fl = layout();
    // Table values are in kbps, convert to bps
    constexpr std::uint32_t kBpsPerKbps = 1000;
    auto const tableBitrate =
      kVersionLayerBitrateTable[static_cast<std::size_t>(fl.versionId())][static_cast<std::size_t>(fl.layer())]
                               [static_cast<std::size_t>(fl.bitrateIndex())] *
      kBpsPerKbps;

    if (tableBitrate != 0 || fl.bitrateIndex() != 0 || _resolvedLength == 0)
    {
      return tableBitrate;
    }

    auto const padding = paddingBytes(*this);

    if (_resolvedLength <= padding)
    {
      return 0;
    }

    auto const unpaddedLength = _resolvedLength - padding;
    auto const bitsPerFrame = static_cast<std::uint64_t>(unpaddedLength) * 8U;
    return static_cast<std::uint32_t>((bitsPerFrame * sampleRate()) / samplesPerFrame());
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
      return (fl.versionId() == VersionId::Ver1) ? kSamplesLayerIIIVer1 : kSamplesLayerIIIVer2;
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

    if (fl.versionId() == VersionId::Ver1)
    {
      offset = (fl.channelMode() == ChannelMode::SingleChannel) ? kXingOffsetVer1Mono : kXingOffsetVer1Stereo;
    }
    else
    {
      offset = (fl.channelMode() == ChannelMode::SingleChannel) ? kXingOffsetVer2Mono : kXingOffsetVer2Stereo;
    }

    // Header is 4 bytes
    offset += kXingHeaderSize;

    if (fl.protectionBit() == Protection::Protected)
    {
      constexpr std::size_t kCrcSize = 2;
      offset += kCrcSize;
    }

    // Bound every read below by the current MPEG frame, not trailing bytes.
    auto const* const base = static_cast<std::uint8_t const*>(_data);
    auto const frameSize = std::min(_size, length());
    auto const bound = [frameSize](std::size_t fieldOffset, std::size_t fieldSize) noexcept
    { return fieldOffset <= frameSize && fieldSize <= frameSize - fieldOffset; };

    if (!bound(offset, sizeof(XingLayout)))
    {
      return {};
    }

    auto const* xing = utility::bytes::tryLayout<XingLayout>(utility::bytes::view(base + offset, sizeof(XingLayout)));

    if (xing == nullptr ||
        (std::memcmp(xing->magic.data(), "Xing", 4) != 0 && std::memcmp(xing->magic.data(), "Info", 4) != 0))
    {
      return {};
    }

    auto info = XingInfo{};
    auto const flags = xing->flags.value();

    std::size_t fieldOffset = offset + sizeof(XingLayout);

    if ((flags & XingLayout::kFlagFrames) != 0)
    {
      if (!bound(fieldOffset, sizeof(boost::endian::big_uint32_buf_t)))
      {
        return info;
      }

      info.frames = utility::layout::viewAt<boost::endian::big_uint32_buf_t>(base, fieldOffset)->value();
      fieldOffset += sizeof(boost::endian::big_uint32_buf_t);
    }

    if ((flags & XingLayout::kFlagBytes) != 0)
    {
      if (!bound(fieldOffset, sizeof(boost::endian::big_uint32_buf_t)))
      {
        return info;
      }

      info.bytes = utility::layout::viewAt<boost::endian::big_uint32_buf_t>(base, fieldOffset)->value();
    }

    return info;
  }
} // namespace ao::media::file::mpeg
