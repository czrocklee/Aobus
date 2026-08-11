// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TrackSession.h"

#include "../DecoderFactory.h"
#include "../StreamingSource.h"
#include "DecoderError.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/utility/StrongTypeFormatter.h>

#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>

namespace ao::audio::detail
{
  namespace
  {
    constexpr auto kPrerollDuration = std::chrono::milliseconds{500};
    constexpr auto kDecodeHighWatermarkThreshold = std::chrono::milliseconds{1500};

    // Obtains a ready decoder from the injected factory when present, otherwise
    // from the production factory. Both paths use the same recoverable result
    // contract; a successful null result is still rejected as a broken factory.
    std::unique_ptr<DecoderSession> makeDecoder(TrackSession::DecoderFactoryFn const& decoderFactory,
                                                std::filesystem::path const& path,
                                                std::optional<SampleEncoding> optOutputEncoding)
    {
      auto decoderRes =
        decoderFactory ? decoderFactory(path, optOutputEncoding) : openDecoderSession(path, optOutputEncoding);

      if (!decoderRes)
      {
        throwDecoderError(decoderRes.error());
      }

      AO_INVARIANT(*decoderRes, "Decoder factory succeeded without a session");

      return std::move(*decoderRes);
    }
  } // namespace

  Result<TrackSession::Inspection> TrackSession::inspect(PlaybackInput const& input,
                                                         DecoderFactoryFn const& decoderFactory)
  {
    try
    {
      auto decoderPtr = makeDecoder(decoderFactory, input.filePath, std::nullopt);

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
        if (auto const seekRes = decoderPtr->seek(initialOffset); !seekRes)
        {
          throwDecoderError(seekRes.error());
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

  TrackSession::OpenedTrack TrackSession::activate(PreparedTrack preparedTrack, OnSourceErrorFn onSourceError)
  {
    AO_INVARIANT(preparedTrack.sourcePtr, "Prepared track has no streaming source");

    auto sourcePtr = std::shared_ptr<StreamingSource>{std::move(preparedTrack.sourcePtr)};
    sourcePtr->activate(std::move(onSourceError));

    return OpenedTrack{
      .sourcePtr = std::move(sourcePtr), .backendFormat = preparedTrack.backendFormat, .info = preparedTrack.info};
  }

  std::unique_ptr<StreamingSource> TrackSession::preparePcmSource(std::unique_ptr<DecoderSession> decoderPtr,
                                                                  DecodedStreamInfo const& info)
  {
    auto streamingSourcePtr =
      std::make_unique<StreamingSource>(std::move(decoderPtr), info, kPrerollDuration, kDecodeHighWatermarkThreshold);

    if (auto const prepareRes = streamingSourcePtr->prepare(); !prepareRes)
    {
      throwDecoderError(prepareRes.error());
    }

    return streamingSourcePtr;
  }
} // namespace ao::audio::detail
