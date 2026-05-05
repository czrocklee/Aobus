// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Types.h>

#include <ao/Error.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace ao::audio
{
  class ISource
  {
  public:
    virtual ~ISource() = default;

    virtual ao::Result<> seek(std::uint32_t positionMs) = 0;
    virtual std::size_t read(std::span<std::byte> output) noexcept = 0;
    virtual bool isDrained() const noexcept = 0;
    virtual std::uint32_t bufferedMs() const noexcept = 0;
  };
} // namespace ao::audio
