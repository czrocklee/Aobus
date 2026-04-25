// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/playback/AudioDecoderSession.h"

#ifdef FFmpeg_FOUND
#include "core/playback/FfmpegDecoderSession.h"
#endif

namespace app::core::playback
{

  void initializeAudioDecoders()
  {
#ifdef FFmpeg_FOUND
    FfmpegDecoderSession::initGlobal();
#endif
  }

  std::unique_ptr<IAudioDecoderSession> createAudioDecoderSession(StreamFormat outputFormat)
  {
#ifdef FFmpeg_FOUND
    return std::make_unique<FfmpegDecoderSession>(outputFormat);
#else
    static_cast<void>(outputFormat);
    return {};
#endif
  }

} // namespace app::core::playback
