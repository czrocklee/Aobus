// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <array>
#include <boost/endian/buffers.hpp>
#include <cstdint>
#include <type_traits>

namespace rs::tag::mpeg::id3v2
{
  struct EncodedSize
  {
    std::array<std::uint8_t, 4> data;
  };

  static_assert(sizeof(EncodedSize) == 4);
  static_assert(alignof(EncodedSize) == 1);
  static_assert(std::is_trivial_v<EncodedSize>);

  inline std::size_t decodeSize(EncodedSize size)
  {
    return (static_cast<std::size_t>(size.data[0] & 0x7F) << 21) |
           (static_cast<std::size_t>(size.data[1] & 0x7F) << 14) |
           (static_cast<std::size_t>(size.data[2] & 0x7F) << 7) |
           (static_cast<std::size_t>(size.data[3] & 0x7F));
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

  inline std::size_t frameSize(V22CommonFrameLayout const& layout) { return layout.size.value(); }

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

  inline std::size_t frameSize(V23CommonFrameLayout const& layout) { return layout.size.value(); }

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

  inline std::size_t frameSize(V24CommonFrameLayout const& layout) { return decodeSize(layout.size); }
}