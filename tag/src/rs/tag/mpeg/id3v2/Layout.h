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

#include <boost/endian/buffers.hpp>
#include <type_traits>
#include <cstdint>
#include <array>

namespace rs::tag::mpeg::id3v2
{
  struct EncodedSize
  {
    std::uint8_t padding3 : 1;
    std::uint8_t size3 : 7;
    std::uint8_t padding2 : 1;
    std::uint8_t size2 : 7;
    std::uint8_t padding1 : 1;
    std::uint8_t size1 : 7;
    std::uint8_t padding0 : 1;
    std::uint8_t size0 : 7;
  };

  static_assert(sizeof(EncodedSize) == 4);
  static_assert(alignof(EncodedSize) == 1);
  static_assert(std::is_trivial_v<EncodedSize>);

  inline std::size_t decodeSize(EncodedSize size)
  {
    return size.size0 + size.size1 * (2 << 7) + size.size2 * (2 << 14) + size.size3 * (2 << 21);
  }

  struct HeaderLayout
  {
    std::array<char, 3> id;
    std::uint8_t majorVersion;
    std::uint8_t minorVersion;

    /*    union
       {
         struct
         {
           std::uint8_t unsync : 1;
           std::uint8_t compression : 1;
           std::uint8_t padding : 6;
         } v22;

         struct
         {
           std::uint8_t unsync : 1;
           std::uint8_t extended : 1;
           std::uint8_t experimental : 1;
           std::uint8_t footer : 1;
           std::uint8_t padding : 4;
         } v23;
       } flags; */

    std::uint8_t flags;
    EncodedSize size;
  };

  static_assert(sizeof(HeaderLayout) == 10);
  static_assert(alignof(HeaderLayout) == 1);
  static_assert(std::is_trivial_v<HeaderLayout>);

  struct V22CommonFrameLayout
  {
    using CommonLayout = V22CommonFrameLayout;
    std::array<char, 3> id;
    boost::endian::big_uint24_buf_t size;
  };

  static_assert(sizeof(V22CommonFrameLayout) == 6);
  static_assert(alignof(V22CommonFrameLayout) == 1);
  static_assert(std::is_trivial_v<V22CommonFrameLayout>);

  enum Encoding : std::uint8_t
  {
    Latin_1 = 0u,
    UCS_2 = 1u
  };

  struct V22TextFrameLayout : V22CommonFrameLayout
  {
    using CommonLayout = V22CommonFrameLayout;
    Encoding encoding;
    // text
  };

  static_assert(sizeof(V22TextFrameLayout) == 7);
  static_assert(alignof(V22TextFrameLayout) == 1);
  static_assert(std::is_trivial_v<V22TextFrameLayout>);

  struct V22CommentFrameLayout : V22CommonFrameLayout
  {
    using CommonLayout = V22CommonFrameLayout;
    Encoding encoding;
    std::array<char, 3> language;
  };

  static_assert(sizeof(V22CommentFrameLayout) == 10);
  static_assert(alignof(V22CommentFrameLayout) == 1);
  static_assert(std::is_trivial_v<V22CommentFrameLayout>);

  enum PictureType : std::uint8_t
  {
    FrontCover = 3
  };

  struct V22PictureFrameLayout : V22CommonFrameLayout
  {
    using CommonLayout = V22CommonFrameLayout;
    Encoding encoding;
    std::array<char, 3> format;
    PictureType type;
    // description
    // data
  };

  static_assert(sizeof(V22PictureFrameLayout) == 11);
  static_assert(alignof(V22PictureFrameLayout) == 1);
  static_assert(std::is_trivial_v<V22PictureFrameLayout>);

  inline std::size_t frameSize(const V22CommonFrameLayout& layout) { return layout.size.value(); }

  struct V23CommonFrameLayout
  {
    using CommonLayout = V23CommonFrameLayout;
    std::array<char, 4> id;
    boost::endian::big_uint32_buf_t size;
    std::array<std::uint8_t, 2> flags;
  };

  static_assert(sizeof(V23CommonFrameLayout) == 10);
  static_assert(alignof(V23CommonFrameLayout) == 1);
  static_assert(std::is_trivial_v<V23CommonFrameLayout>);

  struct V23TextFrameLayout : V23CommonFrameLayout
  {
    using CommonLayout = V23CommonFrameLayout;
    Encoding encoding;
    // text
  };

  static_assert(sizeof(V23TextFrameLayout) == 11);
  static_assert(alignof(V23TextFrameLayout) == 1);
  static_assert(std::is_trivial_v<V23TextFrameLayout>);

  inline std::size_t frameSize(const V23CommonFrameLayout& layout) { return layout.size.value(); }

  struct V24CommonFrameLayout
  {
    using CommonLayout = V24CommonFrameLayout;
    std::array<char, 4> id;
    EncodedSize size;
    std::array<std::uint8_t, 2> flags;
  };

  static_assert(sizeof(V24CommonFrameLayout) == 10);
  static_assert(alignof(V24CommonFrameLayout) == 1);
  static_assert(std::is_trivial_v<V24CommonFrameLayout>);

  inline std::size_t frameSize(const V24CommonFrameLayout& layout) { return decodeSize(layout.size); }
}