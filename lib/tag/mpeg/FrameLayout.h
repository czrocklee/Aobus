// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

namespace ao::tag::mpeg
{
  enum class VersionID : std::uint8_t
  {
    Ver25 = 0b00,
    Reserved = 0b01,
    Ver2 = 0b10,
    Ver1 = 0b11
  };

  enum class LayerDescription : std::uint8_t
  {
    Reserved = 0b00,
    LayerIII = 0b01,
    LayerII = 0b10,
    LayerI = 0b11
  };

  enum class Protection : std::uint8_t
  {
    Protected = 0b0,
    None = 0b1
  };

  enum class ChannelMode : std::uint8_t
  {
    Stereo = 0b00,
    JointStereo = 0b01,
    DualChannel = 0b10,
    SingleChannel = 0b11
  };

  struct FrameLayout
  {
    static constexpr std::uint8_t kSyncMask1 = 0xFF;
    static constexpr std::uint8_t kSyncMask2 = 0x07;
    static constexpr std::uint8_t kSyncShift2 = 5;

    static constexpr std::uint8_t kVersionMask = 0x03;
    static constexpr std::uint8_t kVersionShift = 3;

    static constexpr std::uint8_t kLayerMask = 0x03;
    static constexpr std::uint8_t kLayerShift = 1;

    static constexpr std::uint8_t kProtectionMask = 0x01;

    static constexpr std::uint8_t kBitrateMask = 0x0F;
    static constexpr std::uint8_t kBitrateShift = 4;

    static constexpr std::uint8_t kSamplingMask = 0x03;
    static constexpr std::uint8_t kSamplingShift = 2;

    static constexpr std::uint8_t kPaddingMask = 0x01;
    static constexpr std::uint8_t kPaddingShift = 1;

    static constexpr std::uint8_t kPrivateMask = 0x01;

    static constexpr std::uint8_t kChannelModeMask = 0x03;
    static constexpr std::uint8_t kChannelModeShift = 6;

    static constexpr std::uint8_t kModeExtensionMask = 0x03;
    static constexpr std::uint8_t kModeExtensionShift = 4;

    static constexpr std::uint8_t kCopyrightMask = 0x01;
    static constexpr std::uint8_t kCopyrightShift = 3;

    static constexpr std::uint8_t kOriginalMask = 0x01;
    static constexpr std::uint8_t kOriginalShift = 2;

    static constexpr std::uint8_t kEmphasisMask = 0x03;

    std::array<std::uint8_t, 4> data;

    std::uint8_t sync1() const { return data[0]; }
    std::uint8_t sync2() const { return (data[1] >> kSyncShift2) & kSyncMask2; }
    VersionID versionId() const { return static_cast<VersionID>((data[1] >> kVersionShift) & kVersionMask); }
    LayerDescription layer() const { return static_cast<LayerDescription>((data[1] >> kLayerShift) & kLayerMask); }
    Protection protectionBit() const { return static_cast<Protection>(data[1] & kProtectionMask); }
    std::uint8_t bitrateIndex() const { return (data[2] >> kBitrateShift) & kBitrateMask; }
    std::uint8_t samplingRateIndex() const { return (data[2] >> kSamplingShift) & kSamplingMask; }
    std::uint8_t paddingBit() const { return (data[2] >> kPaddingShift) & kPaddingMask; }
    std::uint8_t privateBit() const { return data[2] & kPrivateMask; }
    ChannelMode channelMode() const { return static_cast<ChannelMode>((data[3] >> kChannelModeShift) & kChannelModeMask); } // NOLINT(readability-magic-numbers)
    std::uint8_t modeExtension() const { return (data[3] >> kModeExtensionShift) & kModeExtensionMask; }                // NOLINT(readability-magic-numbers)
    std::uint8_t copyrightBit() const { return (data[3] >> kCopyrightShift) & kCopyrightMask; }                         // NOLINT(readability-magic-numbers)
    std::uint8_t originalBit() const { return (data[3] >> kOriginalShift) & kOriginalMask; }                           // NOLINT(readability-magic-numbers)
    std::uint8_t emphasis() const { return data[3] & kEmphasisMask; }                                                   // NOLINT(readability-magic-numbers)
  };

  static_assert(sizeof(FrameLayout) == 4);
  static_assert(alignof(FrameLayout) == 1);
  static_assert(std::is_trivial_v<FrameLayout>);
}
