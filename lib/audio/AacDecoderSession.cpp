// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "detail/Mp4PacketSource.h"
#include "detail/OutputFormatValidation.h"
#include <ao/Error.h>
#include <ao/audio/AacDecoderSession.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Format.h>
#include <ao/audio/PcmConverter.h>
#include <ao/audio/detail/AacConfigParser.h>

#include <fdk-aac/FDK_audio.h>
#include <fdk-aac/aacdecoder_lib.h>
#include <fdk-aac/machine_type.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace ao::audio
{
  namespace
  {
    constexpr std::uint8_t kAacPcmBitDepth = 16;
    constexpr std::uint32_t kFallbackFrameSize = 2048;
    constexpr std::uint8_t kFallbackMaxChannels = 8;
  } // namespace

  struct AacDecoderSession::Impl final
  {
    Format requestedOutput;
    DecodedStreamInfo info;
    HANDLE_AACDECODER decoder = nullptr;
    detail::Mp4PacketSource packetSource;
    std::vector<UCHAR> inputBuffer;
    std::vector<INT_PCM> pcmBuffer;
    std::vector<std::int32_t> targetPcmBuffer;

    explicit Impl(Format const& output)
      : requestedOutput{output}
    {
    }

    ~Impl() { closeDecoder(); }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void closeDecoder() noexcept
    {
      if (decoder != nullptr)
      {
        ::aacDecoder_Close(decoder);
        decoder = nullptr;
      }
    }

    Result<> openDecoder()
    {
      closeDecoder();
      decoder = ::aacDecoder_Open(TT_MP4_RAW, 1);

      if (decoder == nullptr)
      {
        return makeError(Error::Code::InitFailed, "Failed to create AAC decoder");
      }

      if (::aacDecoder_SetParam(decoder, AAC_PCM_OUTPUT_CHANNEL_MAPPING, 1) != AAC_DEC_OK)
      {
        return makeError(Error::Code::InitFailed, "Failed to configure AAC channel mapping");
      }

      return {};
    }

    Result<> configureDecoder(std::span<std::byte const> magicCookie)
    {
      if (magicCookie.empty())
      {
        return makeError(Error::Code::FormatRejected, "Missing AAC AudioSpecificConfig");
      }

      inputBuffer.resize(magicCookie.size());
      std::ranges::transform(magicCookie, inputBuffer.begin(), [](std::byte byte) { return static_cast<UCHAR>(byte); });

      auto const configResult = [&]
      {
        auto configData = std::array{inputBuffer.data()};
        auto configSize = std::array{static_cast<UINT>(inputBuffer.size())};
        return ::aacDecoder_ConfigRaw(decoder, configData.data(), configSize.data());
      }();

      if (configResult != AAC_DEC_OK)
      {
        return makeError(Error::Code::InitFailed, "Failed to configure AAC decoder");
      }

      if (auto const* streamInfo = ::aacDecoder_GetStreamInfo(decoder);
          streamInfo != nullptr && (std::cmp_greater(streamInfo->numChannels, kFallbackMaxChannels) ||
                                    std::cmp_greater(streamInfo->frameSize, kFallbackFrameSize)))
      {
        return makeError(Error::Code::InitFailed, "Unsupported AAC stream dimensions");
      }

      refreshStreamInfo();

      auto const streamConfig = detail::parseAudioSpecificConfig(magicCookie);

      if (info.sourceFormat.sampleRate == 0)
      {
        info.sourceFormat.sampleRate = streamConfig.sampleRate;
      }

      if (info.sourceFormat.channels == 0)
      {
        info.sourceFormat.channels = streamConfig.channels;
      }

      applyOutputFormat();

      if (info.sourceFormat.sampleRate == 0 || info.sourceFormat.channels == 0)
      {
        return makeError(Error::Code::InitFailed, "Invalid AAC stream configuration");
      }

      return {};
    }

    Result<> validateRequestedOutput() const
    {
      if (auto const result = detail::validateFixedOutputRequest(requestedOutput, info.outputFormat, "AAC"); !result)
      {
        return std::unexpected{result.error()};
      }

      if (requestedOutput.isFloat)
      {
        return makeError(Error::Code::NotSupported, "AAC float output is not supported");
      }

      if (requestedOutput.bitDepth != 0 && requestedOutput.bitDepth != kAacPcmBitDepth &&
          requestedOutput.bitDepth != 32)
      {
        return makeError(Error::Code::NotSupported, "AAC output is limited to 16-bit PCM or 32-bit padded PCM");
      }

      if (requestedOutput.validBits != 0 && requestedOutput.validBits != kAacPcmBitDepth)
      {
        return makeError(Error::Code::NotSupported, "AAC output valid bits must be 16");
      }

      return {};
    }

    void applyOutputFormat()
    {
      info.outputFormat = info.sourceFormat;

      if (requestedOutput.bitDepth == 32)
      {
        info.outputFormat.bitDepth = 32;
        info.outputFormat.validBits = kAacPcmBitDepth;
      }
    }

    void refreshStreamInfo()
    {
      auto const* const streamInfo = ::aacDecoder_GetStreamInfo(decoder);

      if (streamInfo == nullptr)
      {
        return;
      }

      if (streamInfo->sampleRate > 0)
      {
        info.sourceFormat.sampleRate = static_cast<std::uint32_t>(streamInfo->sampleRate);
      }

      if (streamInfo->numChannels > 0)
      {
        info.sourceFormat.channels = static_cast<std::uint8_t>(streamInfo->numChannels);
      }

      info.sourceFormat.bitDepth = kAacPcmBitDepth;
      info.sourceFormat.validBits = kAacPcmBitDepth;
      info.sourceFormat.isInterleaved = true;
      applyOutputFormat();
      info.isLossy = true;
    }
  };

  AacDecoderSession::AacDecoderSession(Format outputFormat)
    : _implPtr{std::make_unique<Impl>(outputFormat)}
  {
  }

  AacDecoderSession::~AacDecoderSession() = default;

  Result<> AacDecoderSession::open(std::filesystem::path const& filePath)
  {
    close();

    auto failOpen = [this](Error error) -> Result<>
    {
      close();
      return std::unexpected{std::move(error)};
    };

    if (auto const result = _implPtr->openDecoder(); !result)
    {
      return failOpen(result.error());
    }

    if (auto const result = _implPtr->packetSource.open(filePath, "mp4a"); !result)
    {
      auto error = result.error();

      if (error.code == Error::Code::FormatRejected)
      {
        error.code = Error::Code::InitFailed;
      }

      return failOpen(std::move(error));
    }

    if (auto const result = _implPtr->configureDecoder(_implPtr->packetSource.magicCookie()); !result)
    {
      return failOpen(result.error());
    }

    _implPtr->info.duration = _implPtr->packetSource.duration();

    if (auto const result = _implPtr->validateRequestedOutput(); !result)
    {
      return failOpen(result.error());
    }

    return {};
  }

  void AacDecoderSession::close()
  {
    _implPtr->packetSource.close();
    _implPtr->closeDecoder();
    _implPtr->inputBuffer.clear();
    _implPtr->pcmBuffer.clear();
    _implPtr->targetPcmBuffer.clear();
    _implPtr->info = {};
  }

  Result<> AacDecoderSession::seek(std::chrono::milliseconds offset)
  {
    if (auto const result = _implPtr->packetSource.seek(offset); !result)
    {
      return std::unexpected{result.error()};
    }

    flush();
    return {};
  }

  void AacDecoderSession::flush()
  {
    if (_implPtr->decoder != nullptr)
    {
      ::aacDecoder_SetParam(_implPtr->decoder, AAC_TPDEC_CLEAR_BUFFER, 1);
    }
  }

  Result<PcmBlock> AacDecoderSession::readNextBlock()
  {
    if (_implPtr->packetSource.atEnd())
    {
      return PcmBlock{.bytes = {}, .endOfStream = true};
    }

    auto const packet = _implPtr->packetSource.packet();

    if (packet.empty())
    {
      return makeError(Error::Code::DecodeFailed, "Failed to read AAC sample payload");
    }

    _implPtr->inputBuffer.resize(packet.size());
    std::ranges::transform(
      packet, _implPtr->inputBuffer.begin(), [](std::byte byte) { return static_cast<UCHAR>(byte); });

    auto bytesValid = static_cast<UINT>(_implPtr->inputBuffer.size());
    auto const fillResult = [&]
    {
      auto inputData = std::array{_implPtr->inputBuffer.data()};
      auto inputSize = std::array{static_cast<UINT>(_implPtr->inputBuffer.size())};
      return ::aacDecoder_Fill(_implPtr->decoder, inputData.data(), inputSize.data(), &bytesValid);
    }();

    if (fillResult != AAC_DEC_OK)
    {
      return makeError(Error::Code::DecodeFailed, "Failed to fill AAC decoder input");
    }

    if (bytesValid != 0)
    {
      return makeError(Error::Code::DecodeFailed, "AAC decoder did not consume the complete sample");
    }

    auto const* streamInfoBefore = ::aacDecoder_GetStreamInfo(_implPtr->decoder);

    if (streamInfoBefore != nullptr && (std::cmp_greater(streamInfoBefore->numChannels, kFallbackMaxChannels) ||
                                        std::cmp_greater(streamInfoBefore->frameSize, kFallbackFrameSize)))
    {
      return makeError(Error::Code::DecodeFailed, "Unsupported AAC stream dimensions");
    }

    auto const frameSizeBefore = (streamInfoBefore != nullptr && streamInfoBefore->frameSize > 0)
                                   ? static_cast<std::uint32_t>(streamInfoBefore->frameSize)
                                   : kFallbackFrameSize;
    auto const channelsBefore = (streamInfoBefore != nullptr && streamInfoBefore->numChannels > 0)
                                  ? static_cast<std::uint8_t>(streamInfoBefore->numChannels)
                                  : kFallbackMaxChannels;

    _implPtr->pcmBuffer.resize(static_cast<std::size_t>(frameSizeBefore) * channelsBefore);

    auto const decodeResult = ::aacDecoder_DecodeFrame(
      _implPtr->decoder, _implPtr->pcmBuffer.data(), static_cast<INT>(_implPtr->pcmBuffer.size()), 0);

    if (decodeResult != AAC_DEC_OK)
    {
      return makeError(Error::Code::DecodeFailed, "AAC decode failed");
    }

    auto const previousInfo = _implPtr->info;
    _implPtr->refreshStreamInfo();

    if (!(_implPtr->info.outputFormat == previousInfo.outputFormat))
    {
      _implPtr->info = previousInfo;
      return makeError(Error::Code::NotSupported, "AAC stream changed output format during playback");
    }

    if (auto const result = _implPtr->validateRequestedOutput(); !result)
    {
      return std::unexpected{result.error()};
    }

    auto const* streamInfo = ::aacDecoder_GetStreamInfo(_implPtr->decoder);

    if (streamInfo == nullptr || streamInfo->frameSize <= 0 || streamInfo->numChannels <= 0 ||
        std::cmp_greater(streamInfo->frameSize, kFallbackFrameSize) ||
        std::cmp_greater(streamInfo->numChannels, kFallbackMaxChannels))
    {
      return makeError(Error::Code::DecodeFailed, "Invalid AAC stream information");
    }

    auto const frames = static_cast<std::uint32_t>(streamInfo->frameSize);
    auto const channels = static_cast<std::uint8_t>(streamInfo->numChannels);
    auto const samples = static_cast<std::size_t>(frames) * channels;
    auto const firstFrameIndex = _implPtr->packetSource.firstFrameIndex(_implPtr->info.sourceFormat.sampleRate, frames);

    if (samples > _implPtr->pcmBuffer.size())
    {
      return makeError(Error::Code::DecodeFailed, "AAC output exceeded the decode buffer");
    }

    _implPtr->pcmBuffer.resize(samples);
    _implPtr->packetSource.advance();

    if (_implPtr->info.outputFormat.bitDepth == 32)
    {
      _implPtr->targetPcmBuffer.resize(samples);
      PcmConverter::pad<INT_PCM, std::int32_t>(_implPtr->pcmBuffer, _implPtr->targetPcmBuffer, 16);

      auto const bytes = std::as_bytes(std::span{_implPtr->targetPcmBuffer});

      return PcmBlock{
        .bytes = bytes,
        .bitDepth = _implPtr->info.outputFormat.bitDepth,
        .frames = frames,
        .firstFrameIndex = firstFrameIndex,
        .endOfStream = _implPtr->packetSource.atEnd(),
      };
    }

    auto const bytes = std::as_bytes(std::span{_implPtr->pcmBuffer});

    return PcmBlock{
      .bytes = bytes,
      .bitDepth = _implPtr->info.outputFormat.bitDepth,
      .frames = frames,
      .firstFrameIndex = firstFrameIndex,
      .endOfStream = _implPtr->packetSource.atEnd(),
    };
  }

  DecodedStreamInfo AacDecoderSession::streamInfo() const
  {
    return _implPtr->info;
  }
} // namespace ao::audio
