// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <CoreAudioTypes/CoreAudioBaseTypes.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace ao::audio::backend::detail
{
  struct BoundCoreAudioRenderBuffer final
  {
    std::span<std::byte> output{};
    bool valid = false;
  };

  /** @brief Validates client channel geometry and binds null AUHAL storage to staging. */
  BoundCoreAudioRenderBuffer bindCoreAudioRenderBuffer(::AudioBufferList* buffers,
                                                       std::span<std::byte> stagingBuffer,
                                                       std::size_t byteCount,
                                                       std::uint32_t clientChannels) noexcept;
} // namespace ao::audio::backend::detail
