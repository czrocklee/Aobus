// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <ranges>
#include <span>
#include <type_traits>

namespace ao::test::id3v2
{
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
} // namespace ao::test::id3v2
