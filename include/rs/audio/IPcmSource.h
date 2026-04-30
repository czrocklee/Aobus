// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/audio/PlaybackTypes.h>

#include <cstddef>
#include <cstdint>
#include <rs/Error.h>
#include <span>
#include <string>

namespace rs::audio
{

  class IPcmSource
  {
  public:
    virtual ~IPcmSource() = default;

    virtual rs::Result<> seek(std::uint32_t positionMs) = 0;
    virtual std::size_t read(std::span<std::byte> output) noexcept = 0;
    virtual bool isDrained() const noexcept = 0;
    virtual std::uint32_t bufferedMs() const noexcept = 0;
  };

} // namespace rs::audio
