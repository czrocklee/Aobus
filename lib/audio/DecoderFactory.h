// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/SampleEncoding.h>

#include <filesystem>
#include <memory>
#include <optional>

namespace ao::audio
{
  /**
   * @brief Opens a ready decoder session for the supplied audio file.
   *
   * Reports an unsupported extension, missing audio track, or container codec
   * as `NotSupported`; mapping failures remain `IoError`, and malformed MP4
   * structure preserves its media parser error.
   */
  Result<std::unique_ptr<DecoderSession>> openDecoderSession(std::filesystem::path const& filePath,
                                                             std::optional<SampleEncoding> optOutputEncoding);
} // namespace ao::audio
