// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "detail/Mp4PacketSource.h"
#include "detail/OutputFormatValidation.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/audio/AacDecoderSession.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Format.h>
#include <ao/audio/PcmConverter.h>
#include <ao/audio/detail/AacConfigParser.h>
#include <ao/audio/detail/DecoderError.h>

#include <fdk-aac/FDK_audio.h>
#include <fdk-aac/aacdecoder_lib.h>
#include <fdk-aac/machine_type.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

    void openDecoder()
    {
      closeDecoder();
      decoder = ::aacDecoder_Open(TT_MP4_RAW, 1);

      if (decoder == nullptr)
      {
        detail::throwDecoderError(Error::Code::InitFailed, "Failed to create AAC decoder");
      }

      if (::aacDecoder_SetParam(decoder, AAC_PCM_OUTPUT_CHANNEL_MAPPING, 1) != AAC_DEC_OK)
      {
        detail::throwDecoderError(Error::Code::InitFailed, "Failed to configure AAC channel mapping");
      }
    }

    void configureDecoder(std::span<std::byte const> magicCookie)
    {
      if (magicCookie.empty())
      {
        detail::throwDecoderError(Error::Code::FormatRejected, "Missing AAC AudioSpecificConfig");
      }

      // UCHAR and std::byte are both byte-sized, so this is a straight copy.
      inputBuffer.resize(magicCookie.size());
      std::memcpy(inputBuffer.data(), magicCookie.data(), magicCookie.size());

      auto const configResult = [&]
      {
        auto configData = std::array{inputBuffer.data()};
        auto configSize = std::array{static_cast<UINT>(inputBuffer.size())};
        return ::aacDecoder_ConfigRaw(decoder, configData.data(), configSize.data());
      }();

      if (configResult != AAC_DEC_OK)
      {
        detail::throwDecoderError(Error::Code::InitFailed, "Failed to configure AAC decoder");
      }

      if (auto const* streamInfo = ::aacDecoder_GetStreamInfo(decoder);
          streamInfo != nullptr && (std::cmp_greater(streamInfo->numChannels, kFallbackMaxChannels) ||
                                    std::cmp_greater(streamInfo->frameSize, kFallbackFrameSize)))
      {
        detail::throwDecoderError(Error::Code::InitFailed, "Unsupported AAC stream dimensions");
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
        detail::throwDecoderError(Error::Code::InitFailed, "Invalid AAC stream configuration");
      }
    }

    void validateRequestedOutput() const
    {
      if (auto const result = detail::validateFixedOutputRequest(requestedOutput, info.outputFormat, "AAC"); !result)
      {
        detail::throwDecoderError(result.error());
      }

      if (requestedOutput.isFloat)
      {
        detail::throwDecoderError(Error::Code::NotSupported, "AAC float output is not supported");
      }

      if (requestedOutput.bitDepth != 0 && requestedOutput.bitDepth != kAacPcmBitDepth &&
          requestedOutput.bitDepth != 32)
      {
        detail::throwDecoderError(
          Error::Code::NotSupported, "AAC output is limited to 16-bit PCM or 32-bit padded PCM");
      }

      if (requestedOutput.validBits != 0 && requestedOutput.validBits != kAacPcmBitDepth)
      {
        detail::throwDecoderError(Error::Code::NotSupported, "AAC output valid bits must be 16");
      }
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
      info.codec = AudioCodec::Aac;
    }
  };

  AacDecoderSession::AacDecoderSession(Format outputFormat)
    : _implPtr{std::make_unique<Impl>(outputFormat)}
  {
  }

  AacDecoderSession::~AacDecoderSession() = default;

  Result<> AacDecoderSession::openCodec(std::filesystem::path const& filePath)
  {
    try
    {
      _implPtr->openDecoder();

      if (auto const result = _implPtr->packetSource.open(filePath, "mp4a"); !result)
      {
        auto error = result.error();

        if (error.code == Error::Code::FormatRejected)
        {
          error.code = Error::Code::InitFailed;
        }

        detail::throwDecoderError(std::move(error));
      }

      _implPtr->configureDecoder(_implPtr->packetSource.magicCookie());

      _implPtr->info.duration = _implPtr->packetSource.duration();

      _implPtr->validateRequestedOutput();

      return {};
    }
    catch (detail::DecoderException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  void AacDecoderSession::close() noexcept
  {
    _implPtr->packetSource.close();
    _implPtr->closeDecoder();
    _implPtr->inputBuffer.clear();
    _implPtr->pcmBuffer.clear();
    _implPtr->targetPcmBuffer.clear();
    _implPtr->info = {};
  }

  Result<> AacDecoderSession::seek(std::chrono::milliseconds offset) noexcept
  {
    if (auto const result = _implPtr->packetSource.seek(offset); !result)
    {
      return std::unexpected{result.error()};
    }

    flush();
    return {};
  }

  void AacDecoderSession::flush() noexcept
  {
    if (_implPtr->decoder != nullptr)
    {
      ::aacDecoder_SetParam(_implPtr->decoder, AAC_TPDEC_CLEAR_BUFFER, 1);
    }
  }

  Result<PcmBlock> AacDecoderSession::readNextBlock() noexcept
  {
    try
    {
      if (_implPtr->packetSource.atEnd())
      {
        return PcmBlock{.bytes = {}, .endOfStream = true};
      }

      auto const packet = _implPtr->packetSource.packet();

      if (packet.empty())
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "Failed to read AAC sample payload");
      }

      _implPtr->inputBuffer.resize(packet.size());
      std::memcpy(_implPtr->inputBuffer.data(), packet.data(), packet.size());

      auto bytesValid = static_cast<UINT>(_implPtr->inputBuffer.size());
      auto const fillResult = [&]
      {
        auto inputData = std::array{_implPtr->inputBuffer.data()};
        auto inputSize = std::array{static_cast<UINT>(_implPtr->inputBuffer.size())};
        return ::aacDecoder_Fill(_implPtr->decoder, inputData.data(), inputSize.data(), &bytesValid);
      }();

      if (fillResult != AAC_DEC_OK)
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "Failed to fill AAC decoder input");
      }

      if (bytesValid != 0)
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "AAC decoder did not consume the complete sample");
      }

      auto const* streamInfoBefore = ::aacDecoder_GetStreamInfo(_implPtr->decoder);

      if (streamInfoBefore != nullptr && (std::cmp_greater(streamInfoBefore->numChannels, kFallbackMaxChannels) ||
                                          std::cmp_greater(streamInfoBefore->frameSize, kFallbackFrameSize)))
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "Unsupported AAC stream dimensions");
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
        detail::throwDecoderError(Error::Code::DecodeFailed, "AAC decode failed");
      }

      auto const previousInfo = _implPtr->info;
      _implPtr->refreshStreamInfo();

      if (!(_implPtr->info.outputFormat == previousInfo.outputFormat))
      {
        _implPtr->info = previousInfo;
        detail::throwDecoderError(Error::Code::NotSupported, "AAC stream changed output format during playback");
      }

      _implPtr->validateRequestedOutput();

      auto const* streamInfo = ::aacDecoder_GetStreamInfo(_implPtr->decoder);

      if (streamInfo == nullptr || streamInfo->frameSize <= 0 || streamInfo->numChannels <= 0 ||
          std::cmp_greater(streamInfo->frameSize, kFallbackFrameSize) ||
          std::cmp_greater(streamInfo->numChannels, kFallbackMaxChannels))
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "Invalid AAC stream information");
      }

      auto const frames = static_cast<std::uint32_t>(streamInfo->frameSize);
      auto const channels = static_cast<std::uint8_t>(streamInfo->numChannels);
      auto const samples = static_cast<std::size_t>(frames) * channels;
      auto const firstFrameIndex =
        _implPtr->packetSource.firstFrameIndex(_implPtr->info.sourceFormat.sampleRate, frames);

      if (samples > _implPtr->pcmBuffer.size())
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "AAC output exceeded the decode buffer");
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
    catch (detail::DecoderException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  DecodedStreamInfo AacDecoderSession::streamInfo() const noexcept
  {
    return _implPtr->info;
  }
} // namespace ao::audio
