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

#pragma once

#include <type_traits>
#include <cstdint>

namespace rs::tag::mpeg
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
    std::uint8_t sync1 : 8;
    std::uint8_t sync2 : 3;
    VersionID versionId : 2;
    LayerDescription layer : 2;
    Protection protectionBit : 1;
    std::uint8_t bitrateIndex : 4;
    std::uint8_t samplingRateIndex : 2;
    std::uint8_t paddingBit : 1;
    std::uint8_t privateBit : 1;
    ChannelMode channelMode : 2;
    std::uint8_t modeExtension : 2;
    std::uint8_t copyrightBit : 1;
    std::uint8_t originalBit : 1;
    std::uint8_t emphasis : 2;
  };

  static_assert(sizeof(FrameLayout) == 4);
  static_assert(alignof(FrameLayout) == 1);
  static_assert(std::is_trivial_v<FrameLayout>);
}