// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TrackSession.h"

#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/DecoderFactory.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Format.h>
#include <ao/audio/FormatNegotiator.h>
#include <ao/audio/IDecoderSession.h>
#include <ao/audio/ISource.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/StreamingSource.h>
#include <ao/audio/detail/DecoderError.h>

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
    std::unique_ptr<IDecoderSession> makeDecoder(TrackSession::DecoderFactoryFn const& decoderFactory,
                                                 std::filesystem::path const& path,
                                                 Format const& outputFormat)
    {
      if (decoderFactory)
      {
        auto decoderPtr = decoderFactory(path, outputFormat);

        if (!decoderPtr)
        {
          throwDecoderError(
            Error::Code::NotSupported, std::format("No audio decoder available for '{}'", path.string()));
        }

        return decoderPtr;
      }

      auto res = createDecoderSession(path, outputFormat);

      if (!res)
      {
        throwDecoderError(res.error());
      }

      return std::move(*res);
    }
  } // namespace

  Result<TrackSession::OpenedTrack> TrackSession::create(PlaybackInput const& input,
                                                         Device const& device,
                                                         BackendId const& backendId,
                                                         ProfileId const& profileId,
                                                         DecoderFactoryFn const& decoderFactory,
                                                         OnSourceErrorFn onSourceError)
  {
    auto const outputFormat = [] { return Format{.isInterleaved = true}; }();

    try
    {
      auto decoderPtr = makeDecoder(decoderFactory, input.filePath, outputFormat);

      if (auto const openResult = decoderPtr->open(input.filePath); !openResult)
      {
        throwDecoderError(openResult.error());
      }

      auto info = decoderPtr->streamInfo();

      if (info.outputFormat.sampleRate == 0 || info.outputFormat.channels == 0 || info.outputFormat.bitDepth == 0)
      {
        throwDecoderError(Error::Code::InitFailed, "Decoder did not return a valid output format");
      }

      auto backendFormat = Format{};

      negotiateFormat(input.filePath, info, decoderPtr, backendFormat, device, backendId, profileId, decoderFactory);

      info = decoderPtr->streamInfo();
      auto sourcePtr = createPcmSource(std::move(decoderPtr), info, std::move(onSourceError));

      return OpenedTrack{.sourcePtr = std::move(sourcePtr), .backendFormat = backendFormat, .info = info};
    }
    catch (DecoderException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  void TrackSession::negotiateFormat(std::filesystem::path const& path,
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
      return;
    }

    auto const plan = FormatNegotiator::buildPlan(info.sourceFormat, device.capabilities);

    if (plan.requiresResample)
    {
      throwDecoderError(
        Error::Code::NotSupported,
        std::format(
          "{} does not support {} Hz and Aobus has no resampler yet", backendId, info.sourceFormat.sampleRate));
    }

    if (plan.requiresChannelRemap)
    {
      throwDecoderError(Error::Code::NotSupported,
                        std::format("{} does not support {} channels and Aobus has no channel remapper yet",
                                    backendId,
                                    static_cast<std::int32_t>(info.sourceFormat.channels)));
    }

    if (!(plan.decoderOutputFormat == info.sourceFormat))
    {
      decoder->close();
      decoder = makeDecoder(decoderFactory, path, plan.decoderOutputFormat);

      if (auto const reOpenResult = decoder->open(path); !reOpenResult)
      {
        throwDecoderError(reOpenResult.error());
      }
    }

    backendFormat = plan.deviceFormat;
  }

  std::shared_ptr<ISource> TrackSession::createPcmSource(std::unique_ptr<IDecoderSession> decoderPtr,
                                                         DecodedStreamInfo const& info,
                                                         OnSourceErrorFn onSourceError)
  {
    auto streamingSourcePtr = std::make_shared<StreamingSource>(
      std::move(decoderPtr), info, std::move(onSourceError), kPrerollDuration, kDecodeHighWatermarkThreshold);

    if (auto const initResult = streamingSourcePtr->initialize(); !initResult)
    {
      throwDecoderError(initResult.error());
    }

    return streamingSourcePtr;
  }
} // namespace ao::audio::detail
