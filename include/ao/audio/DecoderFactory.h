// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/Format.h>
#include <ao/audio/IDecoderSession.h>

#include <filesystem>
#include <memory>

namespace ao::audio
{
  /**
   * @brief Factory function to create a decoder session for a given file.
   *
   * Reports an unsupported extension or container codec as `NotSupported` and a
   * failure to read the container as `IoError`, so callers can distinguish a
   * file we cannot play from a file we cannot open.
   */
  Result<std::unique_ptr<IDecoderSession>> createDecoderSession(std::filesystem::path const& filePath,
                                                                Format outputFormat);
} // namespace ao::audio
