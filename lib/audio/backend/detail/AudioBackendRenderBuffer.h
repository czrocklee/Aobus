// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/RenderTarget.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace ao::audio::backend::detail
{
  struct PreparedAudioBackendRenderBuffer final
  {
    std::uint32_t renderedFrames = 0;
    std::uint32_t framesProvided = 0;
    std::uint32_t positionFrames = 0;
    bool underrun = false;
    bool drained = false;
  };

  /**
   * @brief Completes a fixed-size native render buffer after a short render.
   *
   * Real PCM remains at the beginning of @p buffer and the unrendered suffix
   * is replaced with silence. Native APIs may therefore consume the complete
   * callback buffer while position advances only by committed real frames.
   */
  PreparedAudioBackendRenderBuffer prepareAudioBackendRenderBuffer(
    std::span<std::byte> buffer,
    std::size_t bytesPerFrame,
    RenderPcmResult const& result) noexcept;
} // namespace ao::audio::backend::detail
