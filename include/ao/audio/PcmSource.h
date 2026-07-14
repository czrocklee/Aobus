// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <chrono>
#include <cstddef>
#include <span>

namespace ao::audio
{
  /**
   * @brief PCM source consumed by one active backend render domain.
   *
   * seek() requires the caller to quiesce concurrent read(), isDrained(), and
   * bufferedDuration() calls. Implementations may own a producer that seek()
   * must stop separately before replacing buffered content.
   */
  class PcmSource
  {
  public:
    virtual ~PcmSource() = default;

    PcmSource(PcmSource const&) = delete;
    PcmSource& operator=(PcmSource const&) = delete;
    PcmSource(PcmSource&&) = delete;
    PcmSource& operator=(PcmSource&&) = delete;

    virtual Result<> seek(std::chrono::milliseconds offset) noexcept = 0;
    virtual std::size_t read(std::span<std::byte> output) noexcept = 0;
    virtual bool isDrained() const noexcept = 0;
    virtual std::chrono::milliseconds bufferedDuration() const noexcept = 0;

  protected:
    PcmSource() = default;
  };
} // namespace ao::audio
