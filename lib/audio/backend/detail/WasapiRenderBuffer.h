// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/RenderTarget.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ao::audio::backend::detail
{
  struct WasapiRenderPacket final
  {
    std::uint32_t renderedFrames = 0;
    std::uint32_t framesToRelease = 0;
    bool underrun = false;
    bool drained = false;
  };

  /**
   * @brief Completes a WASAPI render packet after a RenderTarget short read.
   *
   * WASAPI requires a non-zero ReleaseBuffer call to commit the same frame
   * count requested by GetBuffer. Real PCM remains at the front of @p buffer;
   * any unrendered suffix is zero-filled so only real frames advance playback
   * position. A drained zero-frame result releases the packet without commit.
   */
  inline WasapiRenderPacket prepareWasapiRenderPacket(std::span<std::byte> buffer,
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

    if (renderedFrames == 0 && result.drained)
    {
      return {.renderedFrames = 0, .framesToRelease = 0, .underrun = false, .drained = true};
    }

    if (renderedFrames < requestedFrames)
    {
      auto const renderedBytes = static_cast<std::size_t>(renderedFrames) * bytesPerFrame;
      std::ranges::fill(buffer.subspan(renderedBytes), std::byte{0});
    }

    return {.renderedFrames = renderedFrames,
            .framesToRelease = requestedFrames,
            .underrun = renderedFrames < requestedFrames && !result.drained,
            .drained = result.drained};
  }
} // namespace ao::audio::backend::detail
