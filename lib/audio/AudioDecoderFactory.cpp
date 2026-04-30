// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/audio/AlacDecoderSession.h>
#include <rs/audio/AudioDecoderFactory.h>
#include <rs/audio/FlacDecoderSession.h>
#include <rs/utility/Log.h>

namespace rs::audio
{

  void initializeAudioDecoders()
  {
  }

  std::unique_ptr<IAudioDecoderSession> createAudioDecoderSession(std::filesystem::path const& filePath,
                                                                  AudioFormat outputFormat)
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

} // namespace rs::audio