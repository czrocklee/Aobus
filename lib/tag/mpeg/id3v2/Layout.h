// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/utility/ByteView.h>

#include <boost/endian/buffers.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace ao::tag::mpeg::id3v2
{
  static constexpr std::size_t kId22Size = 3;
  static constexpr std::size_t kId23Size = 4;

  struct EncodedSize
  {
    std::array<std::uint8_t, 4> data;
  };

  static_assert(sizeof(EncodedSize) == 4);
  static_assert(alignof(EncodedSize) == 1);
  static_assert(utility::layout::kIsBinaryLayoutType<EncodedSize>);

  inline std::size_t decodeSize(EncodedSize size)
  {
    static constexpr std::uint8_t kSyncSafeMask = 0x7F;
    static constexpr std::size_t kByteShift3 = 21;
    static constexpr std::size_t kByteShift2 = 14;
    static constexpr std::size_t kByteShift1 = 7;

    return (static_cast<std::size_t>(size.data[0] & kSyncSafeMask) << kByteShift3) |
           (static_cast<std::size_t>(size.data[1] & kSyncSafeMask) << kByteShift2) |
           (static_cast<std::size_t>(size.data[2] & kSyncSafeMask) << kByteShift1) |
           (static_cast<std::size_t>(size.data.back() & kSyncSafeMask));
  }

  struct HeaderLayout
  {
    static constexpr std::size_t kSize = 10;
    std::array<char, kId22Size> id;
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

  static_assert(sizeof(HeaderLayout) == HeaderLayout::kSize);
  static_assert(alignof(HeaderLayout) == 1);
  static_assert(utility::layout::kIsBinaryLayoutType<HeaderLayout>);

  struct V22CommonFrameLayout
  {
    static constexpr std::size_t kSize = 6;
    using CommonLayout = V22CommonFrameLayout;
    std::array<char, kId22Size> id;
    boost::endian::big_uint24_buf_t size;
  };

  static_assert(sizeof(V22CommonFrameLayout) == V22CommonFrameLayout::kSize);
  static_assert(alignof(V22CommonFrameLayout) == 1);
  static_assert(utility::layout::kIsBinaryLayoutType<V22CommonFrameLayout>);

  enum class Encoding : std::uint8_t
  {
    Latin1 = 0U,
    Ucs2 = 1U,
    // ID3v2.4 adds UTF-16BE (without BOM) and UTF-8 text encodings.
    Utf16Be = 2U,
    Utf8 = 3U
  };

  struct V22TextFrameLayout
  {
    static constexpr std::size_t kSize = 7;
    using CommonLayout = V22CommonFrameLayout;

    V22CommonFrameLayout common;
    Encoding encoding;
    // text
  };

  static_assert(sizeof(V22TextFrameLayout) == V22TextFrameLayout::kSize);
  static_assert(alignof(V22TextFrameLayout) == 1);
  static_assert(utility::layout::kIsBinaryLayoutType<V22TextFrameLayout>);

  struct V22CommentFrameLayout
  {
    static constexpr std::size_t kSize = 10;
    using CommonLayout = V22CommonFrameLayout;

    V22CommonFrameLayout common;
    Encoding encoding;
    std::array<char, kId22Size> language;
  };

  static_assert(sizeof(V22CommentFrameLayout) == V22CommentFrameLayout::kSize);
  static_assert(alignof(V22CommentFrameLayout) == 1);
  static_assert(utility::layout::kIsBinaryLayoutType<V22CommentFrameLayout>);

  enum class PictureType : std::uint8_t
  {
    FrontCover = 3
  };

  struct V22PictureFrameLayout
  {
    static constexpr std::size_t kSize = 11;
    using CommonLayout = V22CommonFrameLayout;

    V22CommonFrameLayout common;
    Encoding encoding;
    std::array<char, kId22Size> format;
    PictureType type;
    // description
    // data
  };

  static_assert(sizeof(V22PictureFrameLayout) == V22PictureFrameLayout::kSize);
  static_assert(alignof(V22PictureFrameLayout) == 1);
  static_assert(utility::layout::kIsBinaryLayoutType<V22PictureFrameLayout>);

  inline std::size_t frameSize(V22CommonFrameLayout const& layout)
  {
    return layout.size.value();
  }

  struct V23CommonFrameLayout
  {
    static constexpr std::size_t kSize = 10;
    using CommonLayout = V23CommonFrameLayout;
    std::array<char, kId23Size> id;
    boost::endian::big_uint32_buf_t size;
    std::array<std::uint8_t, 2> flags;
  };

  static_assert(sizeof(V23CommonFrameLayout) == V23CommonFrameLayout::kSize);
  static_assert(alignof(V23CommonFrameLayout) == 1);
  static_assert(utility::layout::kIsBinaryLayoutType<V23CommonFrameLayout>);

  struct V23TextFrameLayout
  {
    static constexpr std::size_t kSize = 11;
    using CommonLayout = V23CommonFrameLayout;

    V23CommonFrameLayout common;
    Encoding encoding;
    // text
  };

  static_assert(sizeof(V23TextFrameLayout) == V23TextFrameLayout::kSize);
  static_assert(alignof(V23TextFrameLayout) == 1);
  static_assert(utility::layout::kIsBinaryLayoutType<V23TextFrameLayout>);

  inline std::size_t frameSize(V23CommonFrameLayout const& layout)
  {
    return layout.size.value();
  }

  struct V24CommonFrameLayout
  {
    static constexpr std::size_t kSize = 10;
    using CommonLayout = V24CommonFrameLayout;
    std::array<char, kId23Size> id;
    EncodedSize size;
    std::array<std::uint8_t, 2> flags;
  };

  static_assert(sizeof(V24CommonFrameLayout) == V24CommonFrameLayout::kSize);
  static_assert(alignof(V24CommonFrameLayout) == 1);
  static_assert(utility::layout::kIsBinaryLayoutType<V24CommonFrameLayout>);

  inline std::size_t frameSize(V24CommonFrameLayout const& layout)
  {
    return decodeSize(layout.size);
  }

  struct V24TextFrameLayout
  {
    static constexpr std::size_t kSize = 11;
    using CommonLayout = V24CommonFrameLayout;

    V24CommonFrameLayout common;
    Encoding encoding;
    // text
  };

  static_assert(sizeof(V24TextFrameLayout) == V24TextFrameLayout::kSize);
  static_assert(alignof(V24TextFrameLayout) == 1);
  static_assert(utility::layout::kIsBinaryLayoutType<V24TextFrameLayout>);
}
