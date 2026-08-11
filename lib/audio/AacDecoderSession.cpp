// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AacDecoderSession.h"

#include "detail/AacConfigParser.h"
#include "detail/DecoderError.h"
#include "detail/DecoderOutputAdapter.h"
#include "detail/Mp4PacketSource.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

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
#include <optional>
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
    detail::DecoderOutputAdapter outputAdapter;
    DecodedStreamInfo info;
    HANDLE_AACDECODER decoder = nullptr;
    detail::Mp4PacketSource packetSource;
    std::vector<UCHAR> inputBuffer;
    std::vector<INT_PCM> pcmBuffer;

    explicit Impl(std::optional<SampleEncoding> optOutputEncoding)
      : outputAdapter{optOutputEncoding}
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

      auto const configRes = [&]
      {
        auto configData = std::array{inputBuffer.data()};
        auto configSize = std::array{static_cast<UINT>(inputBuffer.size())};
        return ::aacDecoder_ConfigRaw(decoder, configData.data(), configSize.data());
      }();

      if (configRes != AAC_DEC_OK)
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

    void applyOutputFormat()
    {
      auto const configuredRes = outputAdapter.configure(info.sourceFormat, SampleEncoding::Signed16Le);

      if (!configuredRes)
      {
        detail::throwDecoderError(configuredRes.error());
      }

      info.outputFormat = *configuredRes;
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

      info.sourceFormat.precisionBits = kAacPcmBitDepth;
      info.sourceFormat.sampleKind = SampleKind::Integer;
      info.isLossy = true;
      info.codec = AudioCodec::Aac;
    }
  };

  AacDecoderSession::AacDecoderSession(std::optional<SampleEncoding> optOutputEncoding)
    : _implPtr{std::make_unique<Impl>(optOutputEncoding)}
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
    _implPtr->outputAdapter.reset();
    _implPtr->info = {};
  }

  // Result error materialization may allocate; DecoderSession intentionally
  // fails fast if an allocation escapes this noexcept boundary.
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

  // Result error materialization and decode buffers may allocate; DecoderSession
  // intentionally fails fast if an allocation escapes this noexcept boundary.
  Result<PcmBlock> AacDecoderSession::readNextBlock() noexcept
  {
    try
    {
      if (_implPtr->packetSource.isAtEnd())
      {
        return PcmBlock{.endOfStream = true};
      }

      auto const packet = _implPtr->packetSource.packet();

      if (packet.empty())
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "Failed to read AAC sample payload");
      }

      _implPtr->inputBuffer.resize(packet.size());
      std::memcpy(_implPtr->inputBuffer.data(), packet.data(), packet.size());

      auto bytesValid = static_cast<UINT>(_implPtr->inputBuffer.size());
      auto const fillRes = [&]
      {
        auto inputData = std::array{_implPtr->inputBuffer.data()};
        auto inputSize = std::array{static_cast<UINT>(_implPtr->inputBuffer.size())};
        return ::aacDecoder_Fill(_implPtr->decoder, inputData.data(), inputSize.data(), &bytesValid);
      }();

      if (fillRes != AAC_DEC_OK)
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

      auto const decodeRes = ::aacDecoder_DecodeFrame(
        _implPtr->decoder, _implPtr->pcmBuffer.data(), static_cast<INT>(_implPtr->pcmBuffer.size()), 0);

      if (decodeRes != AAC_DEC_OK)
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "AAC decode failed");
      }

      auto const previousInfo = _implPtr->info;
      _implPtr->refreshStreamInfo();

      if (!(_implPtr->info.sourceFormat == previousInfo.sourceFormat))
      {
        _implPtr->info = previousInfo;
        detail::throwDecoderError(Error::Code::NotSupported, "AAC stream changed output format during playback");
      }

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

      auto const nativeBytes = std::as_bytes(std::span{_implPtr->pcmBuffer});
      auto convertedRes = _implPtr->outputAdapter.convert(nativeBytes);

      if (!convertedRes)
      {
        detail::throwDecoderError(convertedRes.error());
      }

      return PcmBlock{
        .bytes = *convertedRes,
        .frames = frames,
        .firstFrameIndex = firstFrameIndex,
        .endOfStream = _implPtr->packetSource.isAtEnd(),
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
