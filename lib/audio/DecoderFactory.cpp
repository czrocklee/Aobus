// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/DecoderFactory.h>

#include "AacDecoderSession.h"
#include "AlacDecoderSession.h"
#include "FlacDecoderSession.h"
#include "Mp3DecoderSession.h"
#include "WavDecoderSession.h"
#include <ao/Error.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/media/mp4/SampleDescription.h>
#include <ao/utility/MappedFile.h>

#include <algorithm>
#include <cctype>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>

namespace ao::audio
{
  Result<std::unique_ptr<DecoderSession>> createDecoderSession(std::filesystem::path const& filePath,
                                                               std::optional<SampleEncoding> optOutputEncoding)
  {
    auto ext = filePath.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (ext == ".flac")
    {
      return std::make_unique<FlacDecoderSession>(optOutputEncoding);
    }

    if (ext == ".m4a" || ext == ".mp4")
    {
      auto mappedFile = utility::MappedFile{};

      if (auto const mapRes = mappedFile.map(filePath); !mapRes)
      {
        return makeError(
          Error::Code::IoError, std::format("Failed to map '{}': {}", filePath.string(), mapRes.error().message));
      }

      auto const sampleEntryTypeRes = media::mp4::audioSampleEntryType(mappedFile.bytes());

      if (!sampleEntryTypeRes)
      {
        if (sampleEntryTypeRes.error().code == Error::Code::NotFound)
        {
          return makeError(Error::Code::NotSupported,
                           std::format("MP4 container in '{}' has no supported audio track", filePath.string()));
        }

        return std::unexpected{sampleEntryTypeRes.error()};
      }

      auto const& sampleEntryType = *sampleEntryTypeRes;

      if (sampleEntryType == "alac")
      {
        return std::make_unique<AlacDecoderSession>(optOutputEncoding);
      }

      if (sampleEntryType == "mp4a")
      {
        return std::make_unique<AacDecoderSession>(optOutputEncoding);
      }

      return makeError(Error::Code::NotSupported,
                       std::format("Unsupported MP4 audio codec '{}' in '{}'", sampleEntryType, filePath.string()));
    }

    if (ext == ".mp3")
    {
      return std::make_unique<Mp3DecoderSession>(optOutputEncoding);
    }

    if (ext == ".wav")
    {
      return std::make_unique<WavDecoderSession>(optOutputEncoding);
    }

    return makeError(Error::Code::NotSupported, std::format("Unsupported audio file extension '{}'", ext));
  }
} // namespace ao::audio
