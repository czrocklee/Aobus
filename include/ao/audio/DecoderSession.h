// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/PcmBlock.h>

#include <chrono>
#include <cstdint>
#include <stop_token>

namespace ao::audio
{
  /**
   * @brief Interface for an audio decoding session.
   * Represents an open audio stream that can be sought and decoded.
   *
   * All operations report recoverable failures through Result and never throw
   * to the caller; the methods are noexcept so that an escaping exception
   * (e.g. std::bad_alloc) fail-fast terminates instead of being silently
   * reinterpreted as a decode error.
   */
  class DecoderSession
  {
  public:
    virtual ~DecoderSession() = default;

    DecoderSession(DecoderSession const&) = delete;
    DecoderSession& operator=(DecoderSession const&) = delete;
    DecoderSession(DecoderSession&&) = delete;
    DecoderSession& operator=(DecoderSession&&) = delete;

    /**
     * @brief Seeks to a specific playback position.
     */
    virtual Result<> seek(std::chrono::milliseconds offset) noexcept = 0;

    /// A seek requiring extended decode work must not run on the playback control thread.
    virtual bool needsSeekWorker(std::chrono::milliseconds /*offset*/) const noexcept { return false; }

    /// A decoder requesting a worker must check cancellation between units of work.
    /// The caller discards a cancelled attempt, including its result.
    virtual Result<> seekInterruptibly(std::chrono::milliseconds offset, std::stop_token /*stopToken*/) noexcept
    {
      return seek(offset);
    }

    /// Worker seek after the caller has retired the old read position and all PCM borrows.
    /// Unlike a transactional seek, cancellation or failure may leave the decoder advanced.
    /// The caller must successfully seek again before reading, or retire the session.
    virtual Result<> seekAfterDiscard(std::chrono::milliseconds offset, std::stop_token stopToken) noexcept
    {
      return seekInterruptibly(offset, stopToken);
    }

    virtual bool supportsFrameSeek() const noexcept { return false; }
    virtual Result<> seekFrame(std::uint64_t /*frameIndex*/) noexcept
    {
      return makeError(Error::Code::NotSupported, "Decoder does not support exact frame seeking");
    }

    /**
     * @brief Flushes internal decoder buffers.
     */
    virtual void flush() noexcept = 0;

    /**
     * @brief Decodes and returns the next block of PCM data.
     * @return A PcmBlock if successful, or an error.
     */
    virtual Result<PcmBlock> readNextBlock() noexcept = 0;

    /**
     * @brief Returns information about the decoded stream.
     */
    virtual DecodedStreamInfo streamInfo() const noexcept = 0;

  protected:
    DecoderSession() = default;
  };
} // namespace ao::audio
