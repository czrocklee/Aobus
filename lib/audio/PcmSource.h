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

    /// Success accepts the offset. If isPreparingSeek() is true, keep render stopped until preroll finishes.
    virtual Result<> seek(std::chrono::milliseconds offset) noexcept = 0;

    // Returns whole PCM frames only, as the RenderTarget contract requires.
    virtual std::size_t read(std::span<std::byte> output) noexcept = 0;
    /// True only at EOF with no queued PCM left to render.
    virtual bool isDrained() const noexcept = 0;
    /// Extended seek/preroll is pending; do not start a render consumer yet.
    virtual bool isPreparingSeek() const noexcept { return false; }
    /// Duration of queued PCM, including frames buffered before seek readiness.
    virtual std::chrono::milliseconds bufferedDuration() const noexcept = 0;

  protected:
    PcmSource() = default;
  };
} // namespace ao::audio
