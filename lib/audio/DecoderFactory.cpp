// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "DecoderFactory.h"

#include "AacDecoderSession.h"
#include "AlacDecoderSession.h"
#include "FlacDecoderSession.h"
#include "Mp3DecoderSession.h"
#include "OpusDecoderSession.h"
#include "WavDecoderSession.h"
#include <ao/Error.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/media/mp4/SampleDescription.h>
#include <ao/utility/MappedFile.h>
#include <ao/utility/Path.h>
#include <ao/utility/String.h>

#include <algorithm>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>

namespace ao::audio
{
  namespace
  {
    template<typename Session>
    Result<std::unique_ptr<DecoderSession>> openConcreteSession(std::filesystem::path const& filePath,
                                                                std::optional<SampleEncoding> optOutputEncoding)
    {
      auto sessionRes = Session::open(filePath, optOutputEncoding);

      if (!sessionRes)
      {
        return std::unexpected{sessionRes.error()};
      }

      return std::unique_ptr<DecoderSession>{std::move(*sessionRes)};
    }
  } // namespace

  Result<std::unique_ptr<DecoderSession>> openDecoderSession(std::filesystem::path const& filePath,
                                                             std::optional<SampleEncoding> optOutputEncoding)
  {
    auto ext = utility::pathToUtf8(filePath.extension());
    std::ranges::transform(ext, ext.begin(), utility::toAsciiLower);

    if (ext == ".flac")
    {
      return openConcreteSession<FlacDecoderSession>(filePath, optOutputEncoding);
    }

    if (ext == ".m4a")
    {
      auto mappedFile = utility::MappedFile{};

      if (auto const mapRes = mappedFile.map(filePath); !mapRes)
      {
        return makeError(Error::Code::IoError,
                         std::format("Failed to map '{}': {}", utility::pathToUtf8(filePath), mapRes.error().message));
      }

      auto const sampleEntryTypeRes = media::mp4::audioSampleEntryType(mappedFile.bytes());

      if (!sampleEntryTypeRes)
      {
        if (sampleEntryTypeRes.error().code == Error::Code::NotFound)
        {
          return makeError(
            Error::Code::NotSupported,
            std::format("MP4 container in '{}' has no supported audio track", utility::pathToUtf8(filePath)));
        }

        return std::unexpected{sampleEntryTypeRes.error()};
      }

      auto const& sampleEntryType = *sampleEntryTypeRes;

      if (sampleEntryType == "alac")
      {
        return openConcreteSession<AlacDecoderSession>(filePath, optOutputEncoding);
      }

      if (sampleEntryType == "mp4a")
      {
        return openConcreteSession<AacDecoderSession>(filePath, optOutputEncoding);
      }

      return makeError(
        Error::Code::NotSupported,
        std::format("Unsupported MP4 audio codec '{}' in '{}'", sampleEntryType, utility::pathToUtf8(filePath)));
    }

    if (ext == ".mp3")
    {
      return openConcreteSession<Mp3DecoderSession>(filePath, optOutputEncoding);
    }

    if (ext == ".wav")
    {
      return openConcreteSession<WavDecoderSession>(filePath, optOutputEncoding);
    }

    if (ext == ".opus")
    {
      return openConcreteSession<OpusDecoderSession>(filePath, optOutputEncoding);
    }

    return makeError(Error::Code::NotSupported, std::format("Unsupported audio file extension '{}'", ext));
  }
} // namespace ao::audio
