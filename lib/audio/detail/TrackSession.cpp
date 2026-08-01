// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TrackSession.h"

#include <ao/Error.h>
#include <ao/audio/DecoderFactory.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/PcmSource.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/StreamingSource.h>
#include <ao/audio/detail/DecoderError.h>
#include <ao/utility/StrongTypeFormatter.h>

#include <chrono>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <utility>

namespace ao::audio::detail
{
  namespace
  {
    constexpr auto kPrerollDuration = std::chrono::milliseconds{500};
    constexpr auto kDecodeHighWatermarkThreshold = std::chrono::milliseconds{1500};

    // Obtains a decoder from the injected factory when present, otherwise from the
    // production factory. The injected seam returns a plain pointer (a test that
    // returns null is a deliberate "no decoder", with no IO to diagnose), so its
    // null result becomes NotSupported; the production factory already carries a
    // precise IoError/NotSupported code, which is propagated unchanged.
    std::unique_ptr<DecoderSession> makeDecoder(TrackSession::DecoderFactoryFn const& decoderFactory,
                                                std::filesystem::path const& path,
                                                std::optional<SampleEncoding> optOutputEncoding)
    {
      if (decoderFactory)
      {
        auto decoderPtr = decoderFactory(path, optOutputEncoding);

        if (!decoderPtr)
        {
          throwDecoderError(
            Error::Code::NotSupported, std::format("No audio decoder available for '{}'", path.string()));
        }

        return decoderPtr;
      }

      auto res = createDecoderSession(path, optOutputEncoding);

      if (!res)
      {
        throwDecoderError(res.error());
      }

      return std::move(*res);
    }
  } // namespace

  Result<TrackSession::Inspection> TrackSession::inspect(PlaybackInput const& input,
                                                         DecoderFactoryFn const& decoderFactory)
  {
    try
    {
      auto decoderPtr = makeDecoder(decoderFactory, input.filePath, std::nullopt);

      if (auto const openResult = decoderPtr->open(input.filePath); !openResult)
      {
        throwDecoderError(openResult.error());
      }

      auto info = decoderPtr->streamInfo();

      if (info.sourceFormat.sampleRate == 0 || info.sourceFormat.channels == 0 ||
          info.sourceFormat.precisionBits == 0 || info.outputFormat.encoding == SampleEncoding::Unknown)
      {
        throwDecoderError(Error::Code::InitFailed, "Decoder did not return a valid inspected signal format");
      }

      return Inspection{.info = info};
    }
    catch (DecoderException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  Result<TrackSession::PreparedTrack> TrackSession::prepare(PlaybackInput const& input,
                                                            Inspection const& inspection,
                                                            PcmFormat const& backendFormat,
                                                            DecoderFactoryFn const& decoderFactory,
                                                            std::chrono::milliseconds const initialOffset)
  {
    try
    {
      auto decoderPtr = makeDecoder(decoderFactory, input.filePath, backendFormat.encoding);

      if (auto const openResult = decoderPtr->open(input.filePath); !openResult)
      {
        throwDecoderError(openResult.error());
      }

      auto const info = decoderPtr->streamInfo();

      if (!(info.sourceFormat == inspection.info.sourceFormat))
      {
        throwDecoderError(Error::Code::FormatRejected, "Track signal format changed after inspection");
      }

      if (!(info.outputFormat == backendFormat))
      {
        throwDecoderError(Error::Code::FormatRejected, "Decoder did not produce the PCM format selected by backend");
      }

      if (initialOffset > std::chrono::milliseconds{0})
      {
        if (auto const seekResult = decoderPtr->seek(initialOffset); !seekResult)
        {
          throwDecoderError(seekResult.error());
        }
      }

      auto sourcePtr = preparePcmSource(std::move(decoderPtr), info);

      return PreparedTrack{.sourcePtr = std::move(sourcePtr), .backendFormat = backendFormat, .info = info};
    }
    catch (DecoderException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  Result<TrackSession::OpenedTrack> TrackSession::activate(PreparedTrack preparedTrack, OnSourceErrorFn onSourceError)
  {
    if (!preparedTrack.sourcePtr)
    {
      return makeError(Error::Code::InvalidState, "Prepared track has no streaming source");
    }

    if (auto activated = preparedTrack.sourcePtr->activate(std::move(onSourceError)); !activated)
    {
      return std::unexpected{activated.error()};
    }

    return OpenedTrack{.sourcePtr = std::shared_ptr<PcmSource>{std::move(preparedTrack.sourcePtr)},
                       .backendFormat = preparedTrack.backendFormat,
                       .info = preparedTrack.info};
  }

  std::unique_ptr<StreamingSource> TrackSession::preparePcmSource(std::unique_ptr<DecoderSession> decoderPtr,
                                                                  DecodedStreamInfo const& info)
  {
    auto streamingSourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), info, OnSourceErrorFn{}, kPrerollDuration, kDecodeHighWatermarkThreshold);

    if (auto const prepareResult = streamingSourcePtr->prepare(); !prepareResult)
    {
      throwDecoderError(prepareResult.error());
    }

    return streamingSourcePtr;
  }
} // namespace ao::audio::detail
