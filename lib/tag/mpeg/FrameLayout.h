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
    Ver2_5 = 0b00,
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
    std::array<std::uint8_t, 4> data;

    std::uint8_t sync1() const { return data[0]; }
    std::uint8_t sync2() const { return (data[1] >> 5) & 0x07; }
    VersionID versionId() const { return static_cast<VersionID>((data[1] >> 3) & 0x03); }
    LayerDescription layer() const { return static_cast<LayerDescription>((data[1] >> 1) & 0x03); }
    Protection protectionBit() const { return static_cast<Protection>(data[1] & 0x01); }
    std::uint8_t bitrateIndex() const { return (data[2] >> 4) & 0x0F; }
    std::uint8_t samplingRateIndex() const { return (data[2] >> 2) & 0x03; }
    std::uint8_t paddingBit() const { return (data[2] >> 1) & 0x01; }
    std::uint8_t privateBit() const { return data[2] & 0x01; }
    ChannelMode channelMode() const { return static_cast<ChannelMode>((data[3] >> 6) & 0x03); }
    std::uint8_t modeExtension() const { return (data[3] >> 4) & 0x03; }
    std::uint8_t copyrightBit() const { return (data[3] >> 3) & 0x01; }
    std::uint8_t originalBit() const { return (data[3] >> 2) & 0x01; }
    std::uint8_t emphasis() const { return data[3] & 0x03; }
  };

  static_assert(sizeof(FrameLayout) == 4);
  static_assert(alignof(FrameLayout) == 1);
  static_assert(std::is_trivial_v<FrameLayout>);
}
