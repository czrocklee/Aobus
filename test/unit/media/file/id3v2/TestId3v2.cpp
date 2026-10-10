// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/media/file/id3v2/TestId3v2.h"

#include "lib/media/file/mpeg/id3v2/Layout.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace ao::test::id3v2
{
  namespace
  {
    void appendByte(Bytes& out, std::uint8_t value)
    {
      out.push_back(std::byte{value});
    }
  } // namespace

  std::span<std::byte const> byteSpan(Bytes const& bytes)
  {
    return {bytes.data(), bytes.size()};
  }

  void appendText(Bytes& out, std::string_view text)
  {
    for (auto const character : text)
    {
      out.push_back(std::byte{static_cast<std::uint8_t>(character)});
    }
  }

  void appendUint32Be(Bytes& out, std::uint32_t value)
  {
    out.push_back(std::byte{static_cast<std::uint8_t>(value >> 24)});
    out.push_back(std::byte{static_cast<std::uint8_t>(value >> 16)});
    out.push_back(std::byte{static_cast<std::uint8_t>(value >> 8)});
    out.push_back(std::byte{static_cast<std::uint8_t>(value)});
  }

  void appendSyncSafe(Bytes& out, std::uint32_t value)
  {
    out.push_back(std::byte{static_cast<std::uint8_t>((value >> 21) & 0x7FU)});
    out.push_back(std::byte{static_cast<std::uint8_t>((value >> 14) & 0x7FU)});
    out.push_back(std::byte{static_cast<std::uint8_t>((value >> 7) & 0x7FU)});
    out.push_back(std::byte{static_cast<std::uint8_t>(value & 0x7FU)});
  }

  media::file::mpeg::id3v2::HeaderLayout makeHeader(std::uint8_t majorVersion, std::uint8_t flags, std::size_t bodySize)
  {
    auto header = media::file::mpeg::id3v2::HeaderLayout{};
    header.id = {'I', 'D', '3'};
    header.majorVersion = majorVersion;
    header.flags = flags;
    auto const size = static_cast<std::uint32_t>(bodySize);
    header.size.data = {static_cast<std::uint8_t>((size >> 21) & 0x7FU),
                        static_cast<std::uint8_t>((size >> 14) & 0x7FU),
                        static_cast<std::uint8_t>((size >> 7) & 0x7FU),
                        static_cast<std::uint8_t>(size & 0x7FU)};
    return header;
  }

  void appendFrame(Bytes& body,
                   std::string_view id,
                   bool v24,
                   std::uint8_t formatFlags,
                   Bytes const& content,
                   std::optional<std::size_t> optDeclaredSize)
  {
    appendText(body, id);
    auto const size = static_cast<std::uint32_t>(optDeclaredSize.value_or(content.size()));

    if (v24)
    {
      appendSyncSafe(body, size);
    }
    else
    {
      appendUint32Be(body, size);
    }

    appendByte(body, 0); // status flags
    appendByte(body, formatFlags);
    body.insert(body.end(), content.begin(), content.end());
  }

  Bytes textContent(media::file::mpeg::id3v2::Encoding encoding, std::string_view text)
  {
    auto content = Bytes{};
    appendByte(content, static_cast<std::uint8_t>(encoding));
    appendText(content, text);
    return content;
  }
} // namespace ao::test::id3v2
