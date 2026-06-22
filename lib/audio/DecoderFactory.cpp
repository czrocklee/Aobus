// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Error.h>
#include <ao/audio/AacDecoderSession.h>
#include <ao/audio/AlacDecoderSession.h>
#include <ao/audio/DecoderFactory.h>
#include <ao/audio/FlacDecoderSession.h>
#include <ao/audio/Format.h>
#include <ao/audio/IDecoderSession.h>
#include <ao/audio/Mp3DecoderSession.h>
#include <ao/media/mp4/SampleDescription.h>
#include <ao/utility/MappedFile.h>

#include <filesystem>
#include <format>
#include <memory>

namespace ao::audio
{
  Result<std::unique_ptr<IDecoderSession>> createDecoderSession(std::filesystem::path const& filePath,
                                                                Format outputFormat)
  {
    auto const ext = filePath.extension().string();

    if (ext == ".flac")
    {
      return std::make_unique<FlacDecoderSession>(outputFormat);
    }

    if (ext == ".m4a" || ext == ".mp4")
    {
      auto mappedFile = utility::MappedFile{};

      if (auto const mapResult = mappedFile.map(filePath); !mapResult)
      {
        return makeError(
          Error::Code::IoError, std::format("Failed to map '{}': {}", filePath.string(), mapResult.error().message));
      }

      auto const sampleEntryType = media::mp4::audioSampleEntryType(mappedFile.bytes());

      if (sampleEntryType == "alac")
      {
        return std::make_unique<AlacDecoderSession>(outputFormat);
      }

      if (sampleEntryType == "mp4a")
      {
        return std::make_unique<AacDecoderSession>(outputFormat);
      }

      return makeError(Error::Code::NotSupported,
                       std::format("Unsupported MP4 audio codec '{}' in '{}'", sampleEntryType, filePath.string()));
    }

    if (ext == ".mp3")
    {
      return std::make_unique<Mp3DecoderSession>(outputFormat);
    }

    return makeError(Error::Code::NotSupported, std::format("Unsupported audio file extension '{}'", ext));
  }
} // namespace ao::audio
