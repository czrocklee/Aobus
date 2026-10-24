// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "WasapiRenderBuffer.h"

#include "AudioBackendRenderBuffer.h"
#include <ao/audio/RenderTarget.h>

#include <cstddef>
#include <span>

namespace ao::audio::backend::detail
{
  WasapiRenderPacket prepareWasapiRenderPacket(std::span<std::byte> buffer,
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
