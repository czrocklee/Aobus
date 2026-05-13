// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/DecoderTypes.h>

#include <ao/Error.h>
#include <filesystem>
#include <optional>
#include <string_view>

namespace ao::audio
{
  /**
   * @brief Interface for an audio decoding session.
   * Handles opening a file, seeking, and reading PCM blocks.
   */
  class IDecoderSession
  {
  public:
    virtual ~IDecoderSession() = default;

    /**
     * @brief Opens an audio file for decoding.
     */
    virtual Result<> open(std::filesystem::path const& filePath) = 0;

    /**
     * @brief Closes the current session and releases resources.
     */
    virtual void close() = 0;

    /**
     * @brief Seeks to a specific position in milliseconds.
     */
    virtual Result<> seek(std::uint32_t positionMs) = 0;

    /**
     * @brief Flushes internal decoder buffers.
     */
    virtual void flush() = 0;

    /**
     * @brief Decodes and returns the next block of PCM data.
     * @return A PcmBlock if successful, or an error.
     */
    virtual Result<PcmBlock> readNextBlock() = 0;

    /**
     * @brief Returns information about the decoded stream.
     */
    virtual DecodedStreamInfo streamInfo() const = 0;
  };
} // namespace ao::audio