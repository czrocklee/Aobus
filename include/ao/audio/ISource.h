// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <chrono>
#include <cstddef>
#include <span>

namespace ao::audio
{
  class ISource
  {
  public:
    virtual ~ISource() = default;

    ISource(ISource const&) = delete;
    ISource& operator=(ISource const&) = delete;
    ISource(ISource&&) = delete;
    ISource& operator=(ISource&&) = delete;

    virtual Result<> seek(std::chrono::milliseconds offset) noexcept = 0;
    virtual std::size_t read(std::span<std::byte> output) noexcept = 0;
    virtual bool isDrained() const noexcept = 0;
    virtual std::chrono::milliseconds bufferedDuration() const noexcept = 0;

  protected:
    ISource() = default;
  };
} // namespace ao::audio
