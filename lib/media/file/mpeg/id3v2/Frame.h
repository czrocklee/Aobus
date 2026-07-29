// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "Layout.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ao::media::file::mpeg::id3v2
{
  namespace text
  {
    static constexpr std::uint8_t kUtf8TwoByteHeader = 0xC0;
    static constexpr std::uint8_t kUtf8ThreeByteHeader = 0xE0;
    static constexpr std::uint8_t kUtf8FourByteHeader = 0xF0;
    static constexpr std::uint8_t kUtf8ContinuationHeader = 0x80;
    static constexpr std::uint8_t kUtf8ContinuationMask = 0x3F;
    static constexpr std::size_t kUtf8Shift18 = 18;
    static constexpr std::size_t kUtf8Shift12 = 12;
    static constexpr std::size_t kUtf8Shift6 = 6;
    static constexpr std::uint32_t kUtf16HighSurrogateBegin = 0xD800;
    static constexpr std::uint32_t kUtf16HighSurrogateEnd = 0xDBFF;
    static constexpr std::uint32_t kUtf16LowSurrogateBegin = 0xDC00;
    static constexpr std::uint32_t kUtf16LowSurrogateEnd = 0xDFFF;
    static constexpr std::uint32_t kUtf16SupplementaryBase = 0x10000;
    static constexpr std::uint32_t kUtf8ThreeByteLimit = 0x10000;
    static constexpr std::uint32_t kUtf8MaxCodePoint = 0x10FFFF;
    static constexpr std::uint32_t kReplacementCodePoint = 0xFFFD;

    bool isHighSurrogate(std::uint32_t cp) noexcept;
    bool isLowSurrogate(std::uint32_t cp) noexcept;
    void appendUtf8(std::string& out, std::uint32_t cp);
    std::string latin1ToUtf8(std::span<std::byte const> buf);

    // ID3v2.4 UTF-8 text is already in the target encoding. Return a view after
    // dropping an optional leading BOM (EF BB BF).
    std::string_view utf8View(std::span<std::byte const> buf) noexcept;
    std::string utf8PassThrough(std::span<std::byte const> buf);

    // UCS-2 (ID3v2.3) and UTF-16BE (ID3v2.4) share this decoder: it is BOM-aware
    // and defaults to big-endian when no BOM is present.
    std::string utf16ToUtf8(std::span<std::byte const> buf);
  } // namespace text

  std::string convertToUtf8(std::span<std::byte const> buf, Encoding encoding);
} // namespace ao::media::file::mpeg::id3v2
