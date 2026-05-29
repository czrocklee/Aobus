// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Error.h>
#include <ao/audio/AlacDecoderSession.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Format.h>
#include <ao/audio/PcmConverter.h>
#include <ao/media/mp4/Demuxer.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/MappedFile.h>

#include <alac/ALACAudioTypes.h>
#include <alac/ALACBitUtilities.h>
#include <alac/ALACDecoder.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <vector>

namespace ao::audio
{
  using namespace media::mp4;
  using namespace utility;

  namespace
  {
    [[maybe_unused]] constexpr std::int32_t kSignExtensionMask = ~0x00FFFFFF;
    constexpr std::uint8_t kBytesPer24BitSample = 3;

    std::uint32_t bytesPerSample(std::uint8_t bitDepth) noexcept
    {
      if (bitDepth == 24U)
      {
        return kBytesPer24BitSample;
      }

      if (bitDepth == 32U)
      {
        return 4U;
      }

      return (bitDepth > 16U) ? 4U : 2U;
    }
  } // namespace

  struct AlacDecoderSession::Impl final
  {
    Format requestedOutput;
    DecodedStreamInfo info;

    std::unique_ptr<ALACDecoder> decoderPtr;

    std::uint32_t currentSampleIndex = 0;
    std::uint32_t timescale = 0;

    utility::MappedFile mappedFile;
    std::unique_ptr<Demuxer> demuxerPtr;

    std::vector<std::byte> sourcePcm;
    std::vector<std::byte> targetPcm;

    Impl(Format const& output)
      : requestedOutput{output}
    {
      decoderPtr = std::make_unique<ALACDecoder>();
    }

    std::uint64_t firstFrameIndex(std::uint32_t sampleIndex) const noexcept
    {
      if (!demuxerPtr)
      {
        return 0;
      }

      if (auto const sampleInfo = demuxerPtr->sampleInfo(sampleIndex);
          timescale > 0 && (sampleInfo.startTime > 0 || sampleInfo.duration > 0))
      {
        return (sampleInfo.startTime * info.sourceFormat.sampleRate) / timescale;
      }

      auto const frameLength = decoderPtr->mConfig.frameLength > 0
                                 ? decoderPtr->mConfig.frameLength
                                 : static_cast<std::uint32_t>(kALACDefaultFramesPerPacket);

      return static_cast<std::uint64_t>(sampleIndex) * frameLength;
    }
  };

  AlacDecoderSession::AlacDecoderSession(Format outputFormat)
    : _implPtr{std::make_unique<Impl>(outputFormat)}
  {
  }

  AlacDecoderSession::~AlacDecoderSession() = default;

  Result<> AlacDecoderSession::open(std::filesystem::path const& filePath)
  {
    close();

    if (auto const mapResult = _implPtr->mappedFile.map(filePath); !mapResult)
    {
      return std::unexpected{mapResult.error()};
    }

    _implPtr->demuxerPtr = std::make_unique<Demuxer>(_implPtr->mappedFile.bytes());

    if (auto const demuxResult = _implPtr->demuxerPtr->parseTrack("alac"); !demuxResult)
    {
      return makeError(Error::Code::InitFailed, demuxResult.error().message);
    }

    auto const cookie = _implPtr->demuxerPtr->magicCookie();
    auto const initStatus =
      _implPtr->decoderPtr->Init(layout::asLegacyPtr<std::uint8_t>(cookie), layout::size32(cookie));

    if (initStatus != ALAC_noErr)
    {
      return makeError(Error::Code::InitFailed, "Failed to initialize ALAC decoder");
    }

    auto const& config = _implPtr->decoderPtr->mConfig;

    if (config.sampleRate == 0 || config.numChannels == 0 || config.bitDepth == 0)
    {
      return makeError(Error::Code::InitFailed, "Invalid ALAC stream configuration");
    }

    _implPtr->timescale = _implPtr->demuxerPtr->timescale();
    auto const duration = _implPtr->demuxerPtr->duration();

    if (_implPtr->timescale == 0)
    {
      _implPtr->timescale = config.sampleRate;
    }

    if (_implPtr->timescale > 0)
    {
      _implPtr->info.durationMs =
        static_cast<std::uint32_t>((static_cast<std::uint64_t>(duration) * 1000U) / _implPtr->timescale);
    }

    _implPtr->info.sourceFormat.channels = config.numChannels;
    _implPtr->info.sourceFormat.sampleRate = config.sampleRate;
    _implPtr->info.sourceFormat.bitDepth = config.bitDepth;
    _implPtr->info.sourceFormat.validBits = config.bitDepth;
    _implPtr->info.sourceFormat.isInterleaved = true;

    _implPtr->info.outputFormat = _implPtr->info.sourceFormat;

    if (_implPtr->requestedOutput.bitDepth != 0)
    {
      _implPtr->info.outputFormat.bitDepth = _implPtr->requestedOutput.bitDepth;
      _implPtr->info.outputFormat.validBits = (_implPtr->requestedOutput.validBits != 0)
                                                ? _implPtr->requestedOutput.validBits
                                                : _implPtr->requestedOutput.bitDepth;
    }

    _implPtr->currentSampleIndex = 0;

    return {};
  }

  void AlacDecoderSession::close()
  {
    _implPtr->demuxerPtr.reset();
    _implPtr->mappedFile.unmap();
    _implPtr->currentSampleIndex = 0;
    _implPtr->timescale = 0;
  }

  Result<> AlacDecoderSession::seek(std::uint32_t positionMs)
  {
    if (_implPtr->timescale == 0)
    {
      return makeError(Error::Code::SeekFailed, "Timescale is 0");
    }

    if (!_implPtr->demuxerPtr)
    {
      return makeError(Error::Code::SeekFailed, "ALAC demuxerPtr is not open");
    }

    auto const targetTime = (static_cast<std::uint64_t>(positionMs) * _implPtr->timescale) / 1000U;
    _implPtr->currentSampleIndex = _implPtr->demuxerPtr->sampleIndexAtTime(targetTime);
    return {};
  }

  void AlacDecoderSession::flush()
  {
  }

  Result<PcmBlock> AlacDecoderSession::readNextBlock()
  {
    if (!_implPtr->demuxerPtr || _implPtr->currentSampleIndex >= _implPtr->demuxerPtr->sampleCount())
    {
      return PcmBlock{.bytes = {}, .endOfStream = true};
    }

    auto const firstFrameIndex = _implPtr->firstFrameIndex(_implPtr->currentSampleIndex);
    auto const packet = _implPtr->demuxerPtr->samplePayload(_implPtr->currentSampleIndex);

    if (packet.empty())
    {
      return makeError(Error::Code::DecodeFailed, "Failed to read ALAC sample payload");
    }

    auto const maxFrames = (_implPtr->decoderPtr->mConfig.frameLength > 0)
                             ? _implPtr->decoderPtr->mConfig.frameLength
                             : static_cast<std::uint32_t>(kALACDefaultFramesPerPacket);

    auto const sourceBps = _implPtr->info.sourceFormat.bitDepth;
    auto const targetBps = _implPtr->info.outputFormat.bitDepth;
    auto const channels = _implPtr->info.outputFormat.channels;

    auto const sourceBytesPerFrame = channels * bytesPerSample(sourceBps);
    auto const targetBytesPerFrame = channels * bytesPerSample(targetBps);

    if (sourceBytesPerFrame == 0 || targetBytesPerFrame == 0)
    {
      return makeError(Error::Code::DecodeFailed, "Invalid ALAC format calculation");
    }

    std::uint32_t numFrames = 0;

    // Reuse member buffers to avoid per-block allocations
    if (sourceBps != targetBps)
    {
      _implPtr->sourcePcm.resize(static_cast<std::size_t>(maxFrames) * sourceBytesPerFrame);
      auto bitBuffer = BitBuffer{};
      ::BitBufferInit(&bitBuffer, layout::asLegacyPtr<std::uint8_t>(packet), layout::size32(packet));

      auto const status = _implPtr->decoderPtr->Decode(
        &bitBuffer, layout::asMutablePtr<uint8_t>(std::span{_implPtr->sourcePcm}), maxFrames, channels, &numFrames);

      if (status != 0)
      {
        return makeError(Error::Code::DecodeFailed, "ALAC decode failed");
      }

      _implPtr->targetPcm.resize(static_cast<std::size_t>(numFrames) * targetBytesPerFrame);

      if (sourceBps == 16 && targetBps == 32)
      {
        auto const src = layout::viewArray<std::int16_t>(std::span{_implPtr->sourcePcm});
        auto const dst = layout::viewArrayMutable<std::int32_t>(std::span{_implPtr->targetPcm});
        PcmConverter::pad<std::int16_t, std::int32_t>(src, dst, 16);
      }
      else if (sourceBps == 24 && targetBps == 32)
      {
        auto const dst = layout::viewArrayMutable<std::int32_t>(std::span{_implPtr->targetPcm});
        PcmConverter::unpackS24(_implPtr->sourcePcm, dst, 8);
      }
      else
      {
        return makeError(
          Error::Code::NotSupported, std::format("Unsupported ALAC conversion: {} -> {}", sourceBps, targetBps));
      }

      _implPtr->currentSampleIndex++;

      return PcmBlock{
        .bytes = _implPtr->targetPcm,
        .bitDepth = targetBps,
        .frames = numFrames,
        .firstFrameIndex = firstFrameIndex,
        .endOfStream = (_implPtr->currentSampleIndex >= _implPtr->demuxerPtr->sampleCount()),
      };
    }

    // Direct decode
    _implPtr->targetPcm.resize(static_cast<std::size_t>(maxFrames) * targetBytesPerFrame);

    auto bitBuffer = BitBuffer{};
    ::BitBufferInit(&bitBuffer, layout::asLegacyPtr<std::uint8_t>(packet), layout::size32(packet));

    auto const status = _implPtr->decoderPtr->Decode(
      &bitBuffer, layout::asMutablePtr<uint8_t>(std::span{_implPtr->targetPcm}), maxFrames, channels, &numFrames);

    if (status != 0)
    {
      return makeError(Error::Code::DecodeFailed, "ALAC decode failed");
    }

    _implPtr->targetPcm.resize(static_cast<std::size_t>(numFrames) * targetBytesPerFrame);
    _implPtr->currentSampleIndex++;

    return PcmBlock{
      .bytes = _implPtr->targetPcm,
      .bitDepth = targetBps,
      .frames = numFrames,
      .firstFrameIndex = firstFrameIndex,
      .endOfStream = (_implPtr->currentSampleIndex >= _implPtr->demuxerPtr->sampleCount()),
    };
  }

  DecodedStreamInfo AlacDecoderSession::streamInfo() const
  {
    return _implPtr->info;
  }
} // namespace ao::audio
