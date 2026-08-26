// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "AudioBackendRenderBuffer.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace ao::audio::backend::detail
{
  struct WasapiRenderPacket final
  {
    std::uint32_t renderedFrames = 0;
    std::uint32_t framesToRelease = 0;
    std::uint32_t positionFrames = 0;
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
    if (bytesPerFrame == 0 || (result.drained && result.bytesWritten < bytesPerFrame))
    {
      return {.drained = result.drained};
    }

    auto const prepared = prepareAudioBackendRenderBuffer(buffer, bytesPerFrame, result);
    return {.renderedFrames = prepared.renderedFrames,
            .framesToRelease = prepared.framesProvided,
            .positionFrames = prepared.positionFrames,
            .underrun = prepared.underrun,
            .drained = prepared.drained};
  }
} // namespace ao::audio::backend::detail
