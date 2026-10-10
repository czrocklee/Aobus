// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "lib/media/file/mpeg/id3v2/Layout.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ao::test::id3v2
{
  // Byte and frame-header fixture helpers shared by the ID3v2 reader tests.
  using Bytes = std::vector<std::byte>;

  // Counts the bytes a stored body shrinks to once FF 00 unsynchronisation
  // escape pairs collapse, which is the size an unsynchronised v2.3 frame
  // header declares. Accepts any contiguous byte buffer whose elements are
  // std::byte or std::uint8_t.
  template<std::ranges::contiguous_range R>
  std::size_t deunsynchronisedSize(R const& bytes)
  {
    using Element = std::remove_const_t<std::ranges::range_value_t<R>>;
    auto const view = std::span{bytes};
    auto size = view.size();

    for (std::size_t index = 0; index + 1 < view.size(); ++index)
    {
      if (view[index] == Element{0xFF} && view[index + 1] == Element{})
      {
        --size;
        ++index;
      }
    }

    return size;
  }

  std::span<std::byte const> byteSpan(Bytes const& bytes);

  void appendText(Bytes& out, std::string_view text);

  void appendUint32Be(Bytes& out, std::uint32_t value);

  void appendSyncSafe(Bytes& out, std::uint32_t value);

  media::file::mpeg::id3v2::HeaderLayout makeHeader(std::uint8_t majorVersion,
                                                    std::uint8_t flags,
                                                    std::size_t bodySize);

  // Appends a 10-byte frame header plus content. v2.3 stores a plain
  // big-endian size, v2.4 a syncsafe size; optDeclaredSize overrides the
  // header size when it differs from the stored byte count.
  void appendFrame(Bytes& body,
                   std::string_view id,
                   bool v24,
                   std::uint8_t formatFlags,
                   Bytes const& content,
                   std::optional<std::size_t> optDeclaredSize = std::nullopt);

  Bytes textContent(media::file::mpeg::id3v2::Encoding encoding, std::string_view text);
} // namespace ao::test::id3v2
