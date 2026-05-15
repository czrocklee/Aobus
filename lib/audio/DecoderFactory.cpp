// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/DecoderFactory.h>

#include <ao/audio/AlacDecoderSession.h>
#include <ao/audio/FlacDecoderSession.h>
#include <ao/audio/Format.h>
#include <ao/audio/IDecoderSession.h>

#include <filesystem>
#include <memory>

namespace ao::audio
{
  void initializeDecoders()
  {
  }

  std::unique_ptr<IDecoderSession> createDecoderSession(std::filesystem::path const& filePath, Format outputFormat)
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
} // namespace ao::audio