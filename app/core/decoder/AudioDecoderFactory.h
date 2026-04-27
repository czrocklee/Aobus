// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/decoder/IAudioDecoderSession.h"

#include <filesystem>
#include <memory>

namespace app::core::decoder
{

  /**
   * @brief Global initialization for audio decoders (if needed).
   */
  void initializeAudioDecoders();

  /**
   * @brief Factory function to create a decoder session for a given file.
   */
  std::unique_ptr<IAudioDecoderSession> createAudioDecoderSession(std::filesystem::path const& filePath,
                                                                  AudioFormat outputFormat);

} // namespace app::core::decoder