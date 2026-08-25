// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "backend/detail/AudioBackendRenderBuffer.h"

#include "backend/detail/AudioBackendRenderProgress.h"

#include <ao/audio/RenderTarget.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ao::audio::backend::detail
{
  PreparedAudioBackendRenderBuffer prepareAudioBackendRenderBuffer(
    std::span<std::byte> buffer,
    std::size_t const bytesPerFrame,
    RenderPcmResult const& result) noexcept
  {
    if (bytesPerFrame == 0)
    {
      return {.drained = result.drained};
    }

    auto const requestedFrames = static_cast<std::uint32_t>(buffer.size() / bytesPerFrame);
    auto const renderedFrames =
      static_cast<std::uint32_t>(std::min<std::size_t>(result.bytesWritten / bytesPerFrame, requestedFrames));

    if (renderedFrames < requestedFrames)
    {
      auto const renderedBytes = static_cast<std::size_t>(renderedFrames) * bytesPerFrame;
      std::ranges::fill(buffer.subspan(renderedBytes), std::byte{0});
    }

    return {.renderedFrames = renderedFrames,
            .framesProvided = requestedFrames,
            .positionFrames =
              committedPositionFrames(renderedFrames, result.positionFrameOffset, result.positionFrames),
            .underrun = renderedFrames < requestedFrames && !result.drained,
            .drained = result.drained};
  }
} // namespace ao::audio::backend::detail
