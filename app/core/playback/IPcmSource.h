// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/playback/PlaybackTypes.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace app::core::playback
{

  struct PcmSourceCallbacks final
  {
    void* userData = nullptr;
    void (*onError)(void* userData) noexcept = nullptr;
  };

  class IPcmSource
  {
  public:
    virtual ~IPcmSource() = default;

    virtual std::size_t read(std::span<std::byte> output) noexcept = 0;
    virtual bool isDrained() const noexcept = 0;
    virtual std::uint32_t bufferedMs() const noexcept = 0;
    virtual bool seek(std::uint32_t positionMs) = 0;
    virtual std::string lastError() const = 0;
  };

} // namespace app::core::playback
