// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TrackSession.h"

#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/DecoderFactory.h>
#include <ao/audio/Format.h>
#include <ao/audio/FormatNegotiator.h>
#include <ao/audio/IDecoderSession.h>
#include <ao/audio/ISource.h>
#include <ao/audio/StreamingSource.h>
#include <ao/audio/Types.h>
#include <ao/utility/Log.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
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
    Result<std::unique_ptr<IDecoderSession>> makeDecoder(TrackSession::DecoderFactoryFn const& decoderFactory,
                                                         std::filesystem::path const& path,
                                                         Format const& outputFormat)
    {
      if (decoderFactory)
      {
        auto decoderPtr = decoderFactory(path, outputFormat);

        if (!decoderPtr)
        {
          return makeError(
            Error::Code::NotSupported, std::format("No audio decoder available for '{}'", path.string()));
        }

        return decoderPtr;
      }

      return createDecoderSession(path, outputFormat);
    }
  }

  Result<TrackSession::OpenedTrack> TrackSession::create(PlaybackInput const& input,
                                                         Device const& device,
                                                         BackendId const& backendId,
                                                         ProfileId const& profileId,
                                                         DecoderFactoryFn const& decoderFactory,
                                                         OnSourceErrorFn onSourceError)
  {
    auto const outputFormat = [] { return Format{.isInterleaved = true}; }();

    auto decoderResult = makeDecoder(decoderFactory, input.filePath, outputFormat);

    if (!decoderResult)
    {
      return std::unexpected{decoderResult.error()};
    }

    auto decoderPtr = std::move(*decoderResult);

    if (auto const openResult = decoderPtr->open(input.filePath); !openResult)
    {
      return std::unexpected{openResult.error()};
    }

    auto info = decoderPtr->streamInfo();

    if (info.outputFormat.sampleRate == 0 || info.outputFormat.channels == 0 || info.outputFormat.bitDepth == 0)
    {
      return std::unexpected{
        Error{.code = Error::Code::InitFailed, .message = "Decoder did not return a valid output format"}};
    }

    auto backendFormat = Format{};

    if (auto const negotiateResult = negotiateFormat(
          input.filePath, info, decoderPtr, backendFormat, device, backendId, profileId, decoderFactory);
        !negotiateResult)
    {
      return std::unexpected{negotiateResult.error()};
    }

    info = decoderPtr->streamInfo();
    auto sourceResult = createPcmSource(std::move(decoderPtr), info, std::move(onSourceError));

    if (!sourceResult)
    {
      return std::unexpected{sourceResult.error()};
    }

    return OpenedTrack{.sourcePtr = std::move(*sourceResult), .backendFormat = backendFormat, .info = info};
  }

  Result<> TrackSession::negotiateFormat(std::filesystem::path const& path,
                                         DecodedStreamInfo& info,
                                         std::unique_ptr<IDecoderSession>& decoder,
                                         Format& backendFormat,
                                         Device const& device,
                                         BackendId const& backendId,
                                         ProfileId const& profileId,
                                         DecoderFactoryFn const& decoderFactory)
  {
    if (backendId == kBackendPipeWire && profileId == kProfileShared)
    {
      backendFormat = info.outputFormat;
      AUDIO_LOG_INFO("PipeWire shared mode keeps the stream at {}Hz/{}b/{}ch",
                     backendFormat.sampleRate,
                     static_cast<int>(backendFormat.bitDepth),
                     static_cast<int>(backendFormat.channels));
      return {};
    }

    auto const plan = FormatNegotiator::buildPlan(info.sourceFormat, device.capabilities);

    if (plan.requiresResample)
    {
      return makeError(
        Error::Code::NotSupported,
        std::format(
          "{} does not support {} Hz and Aobus has no resampler yet", backendId, info.sourceFormat.sampleRate));
    }

    if (plan.requiresChannelRemap)
    {
      return makeError(Error::Code::NotSupported,
                       std::format("{} does not support {} channels and Aobus has no channel remapper yet",
                                   backendId,
                                   static_cast<std::int32_t>(info.sourceFormat.channels)));
    }

    AUDIO_LOG_INFO("Negotiated Plan: decoder={}b/{}bits, device={}Hz/{}b, reason: {}",
                   static_cast<int>(plan.decoderOutputFormat.bitDepth),
                   static_cast<int>(plan.decoderOutputFormat.validBits),
                   plan.deviceFormat.sampleRate,
                   static_cast<int>(plan.deviceFormat.bitDepth),
                   plan.reason);

    if (!(plan.decoderOutputFormat == info.sourceFormat))
    {
      decoder->close();
      auto decoderResult = makeDecoder(decoderFactory, path, plan.decoderOutputFormat);

      if (!decoderResult)
      {
        return std::unexpected{decoderResult.error()};
      }

      decoder = std::move(*decoderResult);

      if (auto const reOpenResult = decoder->open(path); !reOpenResult)
      {
        return std::unexpected{reOpenResult.error()};
      }
    }

    backendFormat = plan.deviceFormat;
    return {};
  }

  Result<std::shared_ptr<ISource>> TrackSession::createPcmSource(std::unique_ptr<IDecoderSession> decoderPtr,
                                                                 DecodedStreamInfo const& info,
                                                                 OnSourceErrorFn onSourceError)
  {
    auto streamingSourcePtr = std::make_shared<StreamingSource>(
      std::move(decoderPtr), info, std::move(onSourceError), kPrerollDuration, kDecodeHighWatermarkThreshold);

    if (auto const initResult = streamingSourcePtr->initialize(); !initResult)
    {
      return std::unexpected{initResult.error()};
    }

    return streamingSourcePtr;
  }
} // namespace ao::audio::detail
