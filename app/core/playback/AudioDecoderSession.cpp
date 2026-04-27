// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/playback/FlacDecoderSession.h"
#include "core/playback/AlacDecoderSession.h"

namespace app::core::playback
{

  void initializeAudioDecoders()
  {
  }

  std::unique_ptr<IAudioDecoderSession> createAudioDecoderSession(std::filesystem::path const& filePath, StreamFormat outputFormat)
  {
    auto const ext = filePath.extension().string();

    if (ext == ".flac")
    {
      return std::make_unique<FlacDecoderSession>(outputFormat);
    }

    if (ext == ".m4a" || ext == ".mp4")
    {
      return std::make_unique<AlacDecoderSession>(outputFormat);
    }

    return {};
  }

} // namespace app::core::playback
