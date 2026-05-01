// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/audio/IDecoderSession.h>

#include <filesystem>
#include <memory>

namespace rs::audio
{
  /**
   * @brief Global initialization for audio decoders (if needed).
   */
  void initializeDecoders();

  /**
   * @brief Factory function to create a decoder session for a given file.
   */
  std::unique_ptr<IDecoderSession> createDecoderSession(std::filesystem::path const& filePath,
                                                                  Format outputFormat);
} // namespace rs::audio