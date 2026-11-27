// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/utility/ByteView.h>

#include <boost/endian/buffers.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ao::media::file::mpeg::id3v2
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

  // The high bit of each byte is reserved and must be clear.
  std::optional<std::size_t> tryDecodeSize(EncodedSize size) noexcept;

  struct HeaderLayout
  {
    static constexpr std::size_t kSize = 10;
    std::array<char, kId22Size> id;
    std::uint8_t majorVersion;
    std::uint8_t minorVersion;

    std::uint8_t flags;
    EncodedSize size;
  };

  static_assert(sizeof(HeaderLayout) == HeaderLayout::kSize);
  static_assert(alignof(HeaderLayout) == 1);
  static_assert(utility::layout::kIsBinaryLayoutType<HeaderLayout>);

  enum class Encoding : std::uint8_t
  {
    Latin1 = 0U,
    Ucs2 = 1U,
    // ID3v2.4 adds UTF-16BE (without BOM) and UTF-8 text encodings.
    Utf16Be = 2U,
    Utf8 = 3U
  };

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
} // namespace ao::media::file::mpeg::id3v2
