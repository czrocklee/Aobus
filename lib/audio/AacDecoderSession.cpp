// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/audio/AacDecoderSession.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Format.h>
#include <ao/audio/PcmConverter.h>
#include <ao/media/mp4/Demuxer.h>
#include <ao/utility/MappedFile.h>

#include <fdk-aac/FDK_audio.h>
#include <fdk-aac/aacdecoder_lib.h>
#include <fdk-aac/machine_type.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ao::audio
{
  namespace
  {
    constexpr std::uint8_t kAacPcmBitDepth = 16;
    constexpr std::uint32_t kMsPerSecond = 1000;
    constexpr std::uint32_t kFallbackFrameSize = 2048;
    constexpr std::uint8_t kFallbackMaxChannels = 8;
    constexpr std::uint32_t kAacEscapeObjectType = 31U;
    constexpr std::uint32_t kAacEscapeSampleRateIndex = 15U;
    constexpr std::array<std::uint32_t, 13> kAacSampleRates =
      {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    constexpr std::array<std::uint8_t, 8> kAacChannelCounts = {0, 1, 2, 3, 4, 5, 6, 8};

    struct AacStreamConfig final
    {
      std::uint32_t sampleRate = 0;
      std::uint8_t channels = 0;
    };

    class BitReader final
    {
    public:
      explicit BitReader(std::span<std::byte const> bytes)
        : _bytes{bytes}
      {
      }

      std::optional<std::uint32_t> read(std::uint8_t bitCount) noexcept
      {
        auto value = std::uint32_t{0};

        for (std::uint8_t idx = 0; idx < bitCount; ++idx)
        {
          if (_bitOffset >= _bytes.size() * 8U)
          {
            return std::nullopt;
          }

          auto const byteIndex = _bitOffset / 8U;
          auto const bitIndex = 7U - (_bitOffset % 8U);
          auto const bit = (static_cast<std::uint8_t>(_bytes[byteIndex]) >> bitIndex) & 1U;
          value = (value << 1U) | bit;
          ++_bitOffset;
        }

        return value;
      }

    private:
      std::span<std::byte const> _bytes;
      std::size_t _bitOffset = 0;
    };

    std::uint32_t durationMs(std::uint64_t duration, std::uint32_t timescale) noexcept
    {
      if (timescale == 0)
      {
        return 0;
      }

      return static_cast<std::uint32_t>((duration * kMsPerSecond) / timescale);
    }

    std::optional<std::uint32_t> readAacObjectType(BitReader& reader) noexcept
    {
      auto const optObjectType = reader.read(5);

      if (!optObjectType)
      {
        return std::nullopt;
      }

      if (*optObjectType != kAacEscapeObjectType)
      {
        return optObjectType;
      }

      auto const optExtension = reader.read(6);

      if (!optExtension)
      {
        return std::nullopt;
      }

      return 32U + *optExtension;
    }

    AacStreamConfig parseAudioSpecificConfig(std::span<std::byte const> bytes) noexcept
    {
      auto reader = BitReader{bytes};

      if (!readAacObjectType(reader))
      {
        return {};
      }

      auto const optSampleRateIndex = reader.read(4);

      if (!optSampleRateIndex)
      {
        return {};
      }

      auto config = AacStreamConfig{};

      if (*optSampleRateIndex == kAacEscapeSampleRateIndex)
      {
        if (auto const optExplicitRate = reader.read(24); optExplicitRate)
        {
          config.sampleRate = *optExplicitRate;
        }
      }
      else if (*optSampleRateIndex < kAacSampleRates.size())
      {
        config.sampleRate = kAacSampleRates.at(*optSampleRateIndex);
      }

      if (auto const optChannelConfig = reader.read(4);
          optChannelConfig && *optChannelConfig < kAacChannelCounts.size())
      {
        config.channels = kAacChannelCounts.at(*optChannelConfig);
      }

      return config;
    }
  } // namespace

  struct AacDecoderSession::Impl final
  {
    Format requestedOutput;
    DecodedStreamInfo info;
    HANDLE_AACDECODER decoder = nullptr;
    utility::MappedFile mappedFile;
    std::unique_ptr<media::mp4::Demuxer> demuxerPtr;
    std::vector<UCHAR> inputBuffer;
    std::vector<INT_PCM> pcmBuffer;
    std::vector<std::int32_t> targetPcmBuffer;
    std::uint32_t currentSampleIndex = 0;
    std::uint32_t timescale = 0;

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

      refreshStreamInfo();

      auto const streamConfig = parseAudioSpecificConfig(magicCookie);

      if (info.sourceFormat.sampleRate == 0)
      {
        info.sourceFormat.sampleRate = streamConfig.sampleRate;
      }

      if (info.sourceFormat.channels == 0)
      {
        info.sourceFormat.channels = streamConfig.channels;
      }

      applyOutputFormat();
      return {};
    }

    Result<> validateRequestedOutput() const
    {
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

      if (!requestedOutput.isInterleaved)
      {
        return makeError(Error::Code::NotSupported, "AAC planar output is not supported");
      }

      if (requestedOutput.sampleRate != 0 && info.outputFormat.sampleRate != 0 &&
          requestedOutput.sampleRate != info.outputFormat.sampleRate)
      {
        return makeError(Error::Code::NotSupported, "AAC sample rate conversion is not supported");
      }

      if (requestedOutput.channels != 0 && info.outputFormat.channels != 0 &&
          requestedOutput.channels != info.outputFormat.channels)
      {
        return makeError(Error::Code::NotSupported, "AAC channel remapping is not supported");
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

    std::uint64_t firstFrameIndex(std::uint32_t sampleIndex, std::uint32_t frameSize) const noexcept
    {
      if (!demuxerPtr)
      {
        return 0;
      }

      if (auto const sampleInfo = demuxerPtr->sampleInfo(sampleIndex);
          timescale > 0 && (sampleInfo.startTime > 0 || sampleInfo.duration > 0) && info.sourceFormat.sampleRate > 0)
      {
        return (sampleInfo.startTime * info.sourceFormat.sampleRate) / timescale;
      }

      return static_cast<std::uint64_t>(sampleIndex) * frameSize;
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

    if (auto const result = _implPtr->openDecoder(); !result)
    {
      return std::unexpected{result.error()};
    }

    if (auto const result = _implPtr->mappedFile.map(filePath); !result)
    {
      return std::unexpected{result.error()};
    }

    _implPtr->demuxerPtr = std::make_unique<media::mp4::Demuxer>(_implPtr->mappedFile.bytes());

    if (auto const result = _implPtr->demuxerPtr->parseTrack("mp4a"); !result)
    {
      return makeError(Error::Code::InitFailed, result.error().message);
    }

    if (auto const result = _implPtr->configureDecoder(_implPtr->demuxerPtr->magicCookie()); !result)
    {
      return std::unexpected{result.error()};
    }

    _implPtr->timescale = _implPtr->demuxerPtr->timescale();
    _implPtr->info.durationMs = durationMs(_implPtr->demuxerPtr->duration(), _implPtr->timescale);
    _implPtr->currentSampleIndex = 0;

    if (auto const result = _implPtr->validateRequestedOutput(); !result)
    {
      return std::unexpected{result.error()};
    }

    return {};
  }

  void AacDecoderSession::close()
  {
    _implPtr->demuxerPtr.reset();
    _implPtr->mappedFile.unmap();
    _implPtr->closeDecoder();
    _implPtr->currentSampleIndex = 0;
    _implPtr->timescale = 0;
    _implPtr->info = {};
  }

  Result<> AacDecoderSession::seek(std::uint32_t positionMs)
  {
    if (!_implPtr->demuxerPtr)
    {
      return makeError(Error::Code::SeekFailed, "AAC demuxer is not open");
    }

    if (_implPtr->timescale == 0)
    {
      return makeError(Error::Code::SeekFailed, "Timescale is 0");
    }

    auto const targetTime = (static_cast<std::uint64_t>(positionMs) * _implPtr->timescale) / kMsPerSecond;
    _implPtr->currentSampleIndex = _implPtr->demuxerPtr->sampleIndexAtTime(targetTime);
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
    if (!_implPtr->demuxerPtr || _implPtr->currentSampleIndex >= _implPtr->demuxerPtr->sampleCount())
    {
      return PcmBlock{.bytes = {}, .endOfStream = true};
    }

    auto const sampleIndex = _implPtr->currentSampleIndex;
    auto const packet = _implPtr->demuxerPtr->samplePayload(sampleIndex);

    if (packet.empty())
    {
      return makeError(Error::Code::DecodeFailed, "Failed to read AAC sample payload");
    }

    _implPtr->inputBuffer.resize(packet.size());
    std::ranges::transform(
      packet, _implPtr->inputBuffer.begin(), [](std::byte byte) { return static_cast<UCHAR>(byte); });

    auto const fillResult = [&]
    {
      auto inputData = std::array{_implPtr->inputBuffer.data()};
      auto inputSize = std::array{static_cast<UINT>(_implPtr->inputBuffer.size())};
      auto bytesValid = inputSize.front();
      return ::aacDecoder_Fill(_implPtr->decoder, inputData.data(), inputSize.data(), &bytesValid);
    }();

    if (fillResult != AAC_DEC_OK)
    {
      return makeError(Error::Code::DecodeFailed, "Failed to fill AAC decoder input");
    }

    auto const* streamInfoBefore = ::aacDecoder_GetStreamInfo(_implPtr->decoder);
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

    _implPtr->refreshStreamInfo();

    if (auto const result = _implPtr->validateRequestedOutput(); !result)
    {
      return std::unexpected{result.error()};
    }

    auto const* streamInfo = ::aacDecoder_GetStreamInfo(_implPtr->decoder);

    if (streamInfo == nullptr || streamInfo->frameSize <= 0 || streamInfo->numChannels <= 0)
    {
      return makeError(Error::Code::DecodeFailed, "Invalid AAC stream information");
    }

    auto const frames = static_cast<std::uint32_t>(streamInfo->frameSize);
    auto const channels = static_cast<std::uint8_t>(streamInfo->numChannels);
    auto const samples = static_cast<std::size_t>(frames) * channels;

    if (samples > _implPtr->pcmBuffer.size())
    {
      return makeError(Error::Code::DecodeFailed, "AAC output exceeded the decode buffer");
    }

    _implPtr->pcmBuffer.resize(samples);
    _implPtr->currentSampleIndex++;

    if (_implPtr->info.outputFormat.bitDepth == 32)
    {
      _implPtr->targetPcmBuffer.resize(samples);
      PcmConverter::pad<INT_PCM, std::int32_t>(_implPtr->pcmBuffer, _implPtr->targetPcmBuffer, 16);

      auto const bytes = std::as_bytes(std::span{_implPtr->targetPcmBuffer});

      return PcmBlock{
        .bytes = bytes,
        .bitDepth = _implPtr->info.outputFormat.bitDepth,
        .frames = frames,
        .firstFrameIndex = _implPtr->firstFrameIndex(sampleIndex, frames),
        .endOfStream = (_implPtr->currentSampleIndex >= _implPtr->demuxerPtr->sampleCount()),
      };
    }

    auto const bytes = std::as_bytes(std::span{_implPtr->pcmBuffer});

    return PcmBlock{
      .bytes = bytes,
      .bitDepth = _implPtr->info.outputFormat.bitDepth,
      .frames = frames,
      .firstFrameIndex = _implPtr->firstFrameIndex(sampleIndex, frames),
      .endOfStream = (_implPtr->currentSampleIndex >= _implPtr->demuxerPtr->sampleCount()),
    };
  }

  DecodedStreamInfo AacDecoderSession::streamInfo() const
  {
    return _implPtr->info;
  }
} // namespace ao::audio
