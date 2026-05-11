// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TrackSession.h"
#include <ao/audio/DecoderFactory.h>
#include <ao/audio/FormatNegotiator.h>
#include <ao/audio/MemorySource.h>
#include <ao/audio/StreamingSource.h>
#include <ao/utility/Log.h>
#include <format>

namespace ao::audio::detail
{
  namespace
  {
    constexpr std::uint32_t kPrerollTargetMs = 200;
    constexpr std::uint32_t kDecodeHighWatermarkMs = 750;
    constexpr std::uint64_t kMemoryPcmSourceBudgetBytes = 64ULL * 1024ULL * 1024ULL;

    std::uint64_t bytesPerSecond(Format const& format) noexcept
    {
      if (format.sampleRate == 0 || format.channels == 0 || format.bitDepth == 0)
      {
        return 0;
      }

      constexpr std::uint32_t kBytesPerSample16 = 2;
      constexpr std::uint32_t kBytesPerSample24 = 3;
      constexpr std::uint32_t kBytesPerSample32 = 4;

      std::uint32_t bytesPerSample = kBytesPerSample32;
      if (format.bitDepth <= 16)
      {
        bytesPerSample = kBytesPerSample16;
      }
      else if (format.bitDepth == 24)
      {
        bytesPerSample = kBytesPerSample24;
      }

      return static_cast<std::uint64_t>(format.sampleRate) * format.channels * bytesPerSample;
    }
  }

  TrackSession::Result TrackSession::create(TrackPlaybackDescriptor const& descriptor,
                                            Device const& device,
                                            BackendId const& backendId,
                                            ProfileId const& profileId,
                                            DecoderFactoryFn const& decoderFactory,
                                            OnSourceErrorFn onSourceError)
  {
    auto const outputFormat = [] { return Format{.isInterleaved = true}; }();

    auto decoder = decoderFactory ? decoderFactory(descriptor.filePath, outputFormat)
                                  : createDecoderSession(descriptor.filePath, outputFormat);

    if (decoder == nullptr)
    {
      return {.error = {.message = "No audio decoder backend is available"}};
    }

    if (auto const openResult = decoder->open(descriptor.filePath); !openResult)
    {
      return {.error = openResult.error()};
    }

    auto info = decoder->streamInfo();

    if (info.outputFormat.sampleRate == 0 || info.outputFormat.channels == 0 || info.outputFormat.bitDepth == 0)
    {
      return {.error = {.message = "Decoder did not return a valid output format"}};
    }

    Format backendFormat;
    std::string errorMsg;
    if (!negotiateFormat(
          descriptor.filePath, info, decoder, backendFormat, device, backendId, profileId, decoderFactory, errorMsg))
    {
      return {.error = {.message = errorMsg}};
    }

    info = decoder->streamInfo();
    auto source = createPcmSource(std::move(decoder), info, std::move(onSourceError), errorMsg);
    if (!source)
    {
      return {.error = {.message = errorMsg}};
    }

    return {.source = std::move(source), .backendFormat = backendFormat, .info = info};
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
                             static_cast<int>(info.sourceFormat.channels));
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

  std::shared_ptr<ISource> TrackSession::createPcmSource(std::unique_ptr<IDecoderSession> decoder,
                                                         DecodedStreamInfo const& info,
                                                         OnSourceErrorFn onSourceError,
                                                         std::string& errorMsg)
  {
    if (shouldUseMemoryPcmSource(info))
    {
      auto const memorySource = std::make_shared<MemorySource>(std::move(decoder), info);

      if (auto const initResult = memorySource->initialize(); !initResult)
      {
        errorMsg = initResult.error().message;
        return nullptr;
      }

      return memorySource;
    }

    auto const streamingSource = std::make_shared<StreamingSource>(
      std::move(decoder), info, std::move(onSourceError), kPrerollTargetMs, kDecodeHighWatermarkMs);

    if (auto const initResult = streamingSource->initialize(); !initResult)
    {
      errorMsg = initResult.error().message;
      return nullptr;
    }

    return streamingSource;
  }

  bool TrackSession::shouldUseMemoryPcmSource(DecodedStreamInfo const& info)
  {
    auto const estimatedBytes = static_cast<double>(bytesPerSecond(info.outputFormat)) * (info.durationMs / 1000.0);
    return estimatedBytes > 0 && estimatedBytes <= kMemoryPcmSourceBudgetBytes;
  }
} // namespace ao::audio::detail
