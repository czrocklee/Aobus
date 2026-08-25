// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <AudioToolbox/AudioToolbox.h>

#include <cstddef>
#include <span>

namespace ao::audio::backend::detail
{
  struct BoundCoreAudioRenderBuffer final
  {
    std::span<std::byte> output{};
    bool valid = false;
  };

  /** @brief Binds AUHAL null output storage to the preallocated staging buffer. */
  BoundCoreAudioRenderBuffer bindCoreAudioRenderBuffer(
    ::AudioBufferList* buffers,
    std::span<std::byte> stagingBuffer,
    std::size_t byteCount) noexcept;
} // namespace ao::audio::backend::detail
