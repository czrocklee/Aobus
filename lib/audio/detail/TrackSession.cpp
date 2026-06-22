// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TrackSession.h"

#include <ao/audio/Backend.h>
#include <ao/audio/DecoderFactory.h>
#include <ao/audio/Format.h>
#include <ao/audio/FormatNegotiator.h>
#include <ao/audio/IDecoderSession.h>
#include <ao/audio/ISource.h>
#include <ao/audio/MemorySource.h>
#include <ao/audio/StreamingSource.h>
#include <ao/audio/Types.h>
#include <ao/utility/Log.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <utility>

namespace ao::audio::detail
{
  namespace
  {
    constexpr auto kPrerollDuration = std::chrono::milliseconds{500};
    constexpr auto kDecodeHighWatermarkThreshold = std::chrono::milliseconds{1500};
    // MemorySource synchronously decodes the entire track on the caller's thread
    // during initialization. We disable it by setting the budget to 0 to avoid
    // blocking the GTK main thread. All tracks now use StreamingSource, which
    // decodes on a background thread.
    constexpr std::uint64_t kMemoryPcmSourceBudgetBytes = 0;
  }

  TrackSession::Result TrackSession::create(PlaybackInput const& input,
                                            Device const& device,
                                            BackendId const& backendId,
                                            ProfileId const& profileId,
                                            DecoderFactoryFn const& decoderFactory,
                                            OnSourceErrorFn onSourceError)
  {
    auto const outputFormat = [] { return Format{.isInterleaved = true}; }();

    auto decoderPtr = decoderFactory ? decoderFactory(input.filePath, outputFormat)
                                     : createDecoderSession(input.filePath, outputFormat);

    if (decoderPtr == nullptr)
    {
      return {.error = {.message = "No audio decoder backend is available"}};
    }

    if (auto const openResult = decoderPtr->open(input.filePath); !openResult)
    {
      return {.error = openResult.error()};
    }

    auto info = decoderPtr->streamInfo();

    if (info.outputFormat.sampleRate == 0 || info.outputFormat.channels == 0 || info.outputFormat.bitDepth == 0)
    {
      return {.error = {.message = "Decoder did not return a valid output format"}};
    }

    auto backendFormat = Format{};
    auto errorMsg = std::string{};

    if (!negotiateFormat(
          input.filePath, info, decoderPtr, backendFormat, device, backendId, profileId, decoderFactory, errorMsg))
    {
      return {.error = {.message = errorMsg}};
    }

    info = decoderPtr->streamInfo();
    auto sourcePtr = createPcmSource(std::move(decoderPtr), info, std::move(onSourceError), errorMsg);

    if (!sourcePtr)
    {
      return {.error = {.message = errorMsg}};
    }

    return {.sourcePtr = std::move(sourcePtr), .backendFormat = backendFormat, .info = info};
  }

  bool TrackSession::negotiateFormat(std::filesystem::path const& path,
                                     DecodedStreamInfo& info,
                                     std::unique_ptr<IDecoderSession>& decoder,
                                     Format& backendFormat,
                                     Device const& device,
                                     BackendId const& backendId,
                                     ProfileId const& profileId,
                                     DecoderFactoryFn const& decoderFactory,
                                     std::string& errorMsg)
  {
    if (backendId == kBackendPipeWire && profileId == kProfileShared)
    {
      backendFormat = info.outputFormat;
      AUDIO_LOG_INFO("PipeWire shared mode keeps the stream at {}Hz/{}b/{}ch",
                     backendFormat.sampleRate,
                     static_cast<int>(backendFormat.bitDepth),
                     static_cast<int>(backendFormat.channels));
      return true;
    }

    auto const plan = FormatNegotiator::buildPlan(info.sourceFormat, device.capabilities);

    if (plan.requiresResample)
    {
      errorMsg = std::format(
        "{} does not support {} Hz and Aobus has no resampler yet", backendId, info.sourceFormat.sampleRate);
      return false;
    }

    if (plan.requiresChannelRemap)
    {
      errorMsg = std::format("{} does not support {} channels and Aobus has no channel remapper yet",
                             backendId,
                             static_cast<std::int32_t>(info.sourceFormat.channels));
      return false;
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
      decoder = decoderFactory ? decoderFactory(path, plan.decoderOutputFormat)
                               : createDecoderSession(path, plan.decoderOutputFormat);

      if (!decoder)
      {
        errorMsg = "Failed to re-open decoder with negotiated format";
        return false;
      }

      if (auto const reOpenResult = decoder->open(path); !reOpenResult)
      {
        errorMsg = reOpenResult.error().message;
        return false;
      }
    }

    backendFormat = plan.deviceFormat;
    return true;
  }

  std::shared_ptr<ISource> TrackSession::createPcmSource(std::unique_ptr<IDecoderSession> decoderPtr,
                                                         DecodedStreamInfo const& info,
                                                         OnSourceErrorFn onSourceError,
                                                         std::string& errorMsg)
  {
    if (shouldUseMemoryPcmSource(info))
    {
      auto const memorySourcePtr = std::make_shared<MemorySource>(std::move(decoderPtr), info);

      if (auto const initResult = memorySourcePtr->initialize(); !initResult)
      {
        errorMsg = initResult.error().message;
        return nullptr;
      }

      return memorySourcePtr;
    }

    auto const streamingSourcePtr = std::make_shared<StreamingSource>(
      std::move(decoderPtr), info, std::move(onSourceError), kPrerollDuration, kDecodeHighWatermarkThreshold);

    if (auto const initResult = streamingSourcePtr->initialize(); !initResult)
    {
      errorMsg = initResult.error().message;
      return nullptr;
    }

    return streamingSourcePtr;
  }

  bool TrackSession::shouldUseMemoryPcmSource(DecodedStreamInfo const& info)
  {
    if (info.outputFormat.sampleRate == 0)
    {
      return false;
    }

    auto const estimatedBytes = static_cast<double>(durationToSamples(info.duration, info.outputFormat.sampleRate)) *
                                (static_cast<double>(bytesPerSecond(info.outputFormat)) / info.outputFormat.sampleRate);
    return estimatedBytes > 0 && estimatedBytes <= kMemoryPcmSourceBudgetBytes;
  }
} // namespace ao::audio::detail
