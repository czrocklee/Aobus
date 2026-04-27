// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/decoder/DecoderTypes.h"

#include <filesystem>
#include <optional>
#include <string_view>

namespace app::core::decoder
{

  /**
   * @brief Interface for an audio decoding session.
   * Handles opening a file, seeking, and reading PCM blocks.
   */
  class IAudioDecoderSession
  {
  public:
    virtual ~IAudioDecoderSession() = default;

    /**
     * @brief Opens an audio file for decoding.
     */
    virtual bool open(std::filesystem::path const& filePath) = 0;

    /**
     * @brief Closes the current session and releases resources.
     */
    virtual void close() = 0;

    /**
     * @brief Seeks to a specific position in milliseconds.
     */
    virtual bool seek(std::uint32_t positionMs) = 0;

    /**
     * @brief Flushes internal decoder buffers.
     */
    virtual void flush() = 0;

    /**
     * @brief Decodes and returns the next block of PCM data.
     * @return A PcmBlock if successful, or std::nullopt on error.
     */
    virtual std::optional<PcmBlock> readNextBlock() = 0;

    /**
     * @brief Returns information about the decoded stream.
     */
    virtual DecodedStreamInfo streamInfo() const = 0;

    /**
     * @brief Returns a human-readable description of the last error.
     */
    virtual std::string_view lastError() const noexcept = 0;
  };

} // namespace app::core::decoder