// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "RenderPath.h"

#include <ao/audio/RenderTarget.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ao::audio::detail
{
  void RenderProgress::advance(std::size_t bytes) noexcept
  {
    bytesWritten += bytes;
    positionBytes += bytes;
  }

  void RenderProgress::startNewPositionSegment() noexcept
  {
    positionStartBytes = bytesWritten;
    positionBytes = 0;
  }

  RenderPcmResult RenderProgress::result(std::atomic<std::uint32_t> const& engineFrameBytes,
                                         bool drained) const noexcept
  {
    auto frameCount = [&engineFrameBytes](std::size_t bytes) noexcept
    {
      auto const bytesPerFrame = engineFrameBytes.load(std::memory_order_relaxed);

      if (bytesPerFrame == 0U)
      {
        return std::uint32_t{0};
      }

      auto const frames = bytes / bytesPerFrame;
      return static_cast<std::uint32_t>(std::min<std::size_t>(frames, std::numeric_limits<std::uint32_t>::max()));
    };

    return {.bytesWritten = bytesWritten,
            .positionFrameOffset = frameCount(positionStartBytes),
            .positionFrames = frameCount(positionBytes),
            .drained = drained};
  }
} // namespace ao::audio::detail
