// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <algorithm>
#include <cstdint>

namespace ao::audio::backend::detail
{
  constexpr std::uint32_t committedPositionFrames(std::uint64_t committedFrames,
                                                  std::uint32_t positionFrameOffset,
                                                  std::uint32_t positionFrames) noexcept
  {
    if (committedFrames <= positionFrameOffset)
    {
      return 0;
    }

    auto const committedAfterOffset = committedFrames - positionFrameOffset;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(committedAfterOffset, positionFrames));
  }
} // namespace ao::audio::backend::detail
