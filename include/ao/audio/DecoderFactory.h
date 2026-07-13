// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/Format.h>

#include <filesystem>
#include <memory>

namespace ao::audio
{
  /**
   * @brief Factory function to create a decoder session for a given file.
   *
   * Reports an unsupported extension, missing audio track, or container codec
   * as `NotSupported`; mapping failures remain `IoError`, and malformed MP4
   * structure preserves its media parser error.
   */
  Result<std::unique_ptr<DecoderSession>> createDecoderSession(std::filesystem::path const& filePath,
                                                               Format outputFormat);
} // namespace ao::audio
