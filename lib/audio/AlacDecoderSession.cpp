// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <ao/audio/AlacDecoderSession.h>

#include <alac/ALACAudioTypes.h>
#include <alac/ALACBitUtilities.h>
#include <alac/ALACDecoder.h>

#include <ao/audio/PcmConverter.h>
#include <ao/media/mp4/Demuxer.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/MappedFile.h>

namespace ao::audio
{
  using namespace ao::media::mp4;
  using namespace ao::utility;

  namespace
  {
    constexpr std::int32_t kSignExtensionMask = ~0x00FFFFFF;
    constexpr std::uint8_t kBytesPer24BitSample = 3;

    std::uint32_t bytesPerSample(std::uint8_t bitDepth) noexcept
    {
      if (bitDepth == 24U)
      {
        return kBytesPer24BitSample;
      }

      if (bitDepth == 32U)
      {
        return 4;
      }

      return (bitDepth > 16U) ? 4U : 2U;
    }
  } // namespace

  struct AlacDecoderSession::Impl
  {
    Format requestedOutput;
    DecodedStreamInfo info;

    std::unique_ptr<ALACDecoder> decoder;

    std::uint32_t currentSampleIndex = 0;
    std::uint32_t timescale = 0;

    ao::utility::MappedFile mappedFile;
    std::unique_ptr<Demuxer> demuxer;

    Impl(Format const& output)
      : requestedOutput{output}
    {
      decoder = std::make_unique<ALACDecoder>();
    }
  };

  AlacDecoderSession::AlacDecoderSession(Format outputFormat)
    : _impl{std::make_unique<Impl>(outputFormat)}
  {
  }

  AlacDecoderSession::~AlacDecoderSession() = default;

  ao::Result<> AlacDecoderSession::open(std::filesystem::path const& filePath)
  {
    close();

    if (auto const mapResult = _impl->mappedFile.map(filePath); !mapResult)
    {
      return std::unexpected(mapResult.error());
    }

    _impl->demuxer = std::make_unique<Demuxer>(_impl->mappedFile.bytes());
    auto const demuxError = _impl->demuxer->parseTrack("alac");

    if (!demuxError.empty())
    {
      return ao::makeError(ao::Error::Code::InitFailed, demuxError);
    }

    auto const cookie = _impl->demuxer->magicCookie();
    auto const initStatus = _impl->decoder->Init(layout::asLegacyPtr<std::uint8_t>(cookie), layout::size32(cookie));

    if (initStatus != ALAC_noErr)
    {
      return ao::makeError(ao::Error::Code::InitFailed, "Failed to initialize ALAC decoder");
    }

    auto const& config = _impl->decoder->mConfig;

    if (config.sampleRate == 0 || config.numChannels == 0 || config.bitDepth == 0)
    {
      return ao::makeError(ao::Error::Code::InitFailed, "Invalid ALAC stream configuration");
    }

    _impl->timescale = _impl->demuxer->timescale();
    auto const duration = _impl->demuxer->duration();

    if (_impl->timescale == 0)
    {
      _impl->timescale = config.sampleRate;
    }

    if (_impl->timescale > 0)
    {
      _impl->info.durationMs =
        static_cast<std::uint32_t>((static_cast<std::uint64_t>(duration) * 1000) / _impl->timescale);
    }

    _impl->info.sourceFormat.channels = config.numChannels;
    _impl->info.sourceFormat.sampleRate = config.sampleRate;
    _impl->info.sourceFormat.bitDepth = config.bitDepth;
    _impl->info.sourceFormat.validBits = config.bitDepth;
    _impl->info.sourceFormat.isInterleaved = true;

    _impl->info.outputFormat = _impl->info.sourceFormat;

    if (_impl->requestedOutput.bitDepth != 0)
    {
      _impl->info.outputFormat.bitDepth = _impl->requestedOutput.bitDepth;
      _impl->info.outputFormat.validBits =
        (_impl->requestedOutput.validBits != 0) ? _impl->requestedOutput.validBits : _impl->requestedOutput.bitDepth;
    }

    _impl->currentSampleIndex = 0;

    return {};
  }

  void AlacDecoderSession::close()
  {
    _impl->demuxer.reset();
    _impl->mappedFile.unmap();
    _impl->currentSampleIndex = 0;
    _impl->timescale = 0;
  }

  ao::Result<> AlacDecoderSession::seek(std::uint32_t /*positionMs*/)
  {
    if (_impl->timescale == 0)
    {
      return ao::makeError(ao::Error::Code::SeekFailed, "Timescale is 0");
    }

    _impl->currentSampleIndex = 0;
    return {};
  }

  void AlacDecoderSession::flush()
  {
  }

  ao::Result<PcmBlock> AlacDecoderSession::readNextBlock()
  {
    if (!_impl->demuxer || _impl->currentSampleIndex >= _impl->demuxer->sampleCount())
    {
      return PcmBlock{.bytes = {}, .endOfStream = true};
    }

    auto const packet = _impl->demuxer->getSamplePayload(_impl->currentSampleIndex);

    if (packet.empty())
    {
      return ao::makeError(ao::Error::Code::DecodeFailed, "Failed to read ALAC sample payload");
    }

    auto const maxFrames = (_impl->decoder->mConfig.frameLength > 0)
                             ? _impl->decoder->mConfig.frameLength
                             : static_cast<std::uint32_t>(kALACDefaultFramesPerPacket);

    auto const sourceBps = _impl->info.sourceFormat.bitDepth;
    auto const targetBps = _impl->info.outputFormat.bitDepth;
    auto const channels = _impl->info.outputFormat.channels;

    auto const sourceBytesPerFrame = channels * bytesPerSample(sourceBps);
    auto const targetBytesPerFrame = channels * bytesPerSample(targetBps);

    if (sourceBytesPerFrame == 0 || targetBytesPerFrame == 0)
    {
      return ao::makeError(ao::Error::Code::DecodeFailed, "Invalid ALAC format calculation");
    }

    std::uint32_t numFrames = 0;

    // If we need conversion (e.g. 24 -> 32), decode to temp buffer first
    if (sourceBps != targetBps)
    {
      std::vector<std::byte> sourcePcm(static_cast<std::size_t>(maxFrames) * sourceBytesPerFrame);
      auto bitBuffer = BitBuffer{};
      ::BitBufferInit(&bitBuffer, layout::asLegacyPtr<std::uint8_t>(packet), layout::size32(packet));

      auto const status = _impl->decoder->Decode(
        &bitBuffer, layout::asMutablePtr<uint8_t>(std::span{sourcePcm}), maxFrames, channels, &numFrames);

      if (status != 0)
      {
        return ao::makeError(ao::Error::Code::DecodeFailed, "ALAC decode failed");
      }

      std::vector<std::byte> targetPcm(static_cast<std::size_t>(numFrames) * targetBytesPerFrame);

      if (sourceBps == 16 && targetBps == 32)
      {
        auto const src = layout::viewArray<std::int16_t>(std::span{sourcePcm});
        auto const dst = layout::viewArrayMutable<std::int32_t>(std::span{targetPcm});
        pcm::Converter::pad<std::int16_t, std::int32_t>(src, dst, 16);
      }
      else if (sourceBps == 24 && targetBps == 32)
      {
        auto const dst = layout::viewArrayMutable<std::int32_t>(std::span{targetPcm});
        pcm::Converter::unpackS24(sourcePcm, dst, 8);
      }
      else
      {
        return ao::makeError(
          ao::Error::Code::NotSupported, std::format("Unsupported ALAC conversion: {} -> {}", sourceBps, targetBps));
      }

      auto block = PcmBlock{};
      block.bytes = std::move(targetPcm);
      block.frames = numFrames;
      block.bitDepth = targetBps;
      _impl->currentSampleIndex++;
      block.endOfStream = (_impl->currentSampleIndex >= _impl->demuxer->sampleCount());
      return block;
    }

    // Direct decode
    std::vector<std::byte> decodedPcm(static_cast<std::size_t>(maxFrames) * targetBytesPerFrame);

    auto bitBuffer = BitBuffer{};
    ::BitBufferInit(&bitBuffer, layout::asLegacyPtr<std::uint8_t>(packet), layout::size32(packet));

    auto const status = _impl->decoder->Decode(
      &bitBuffer, layout::asMutablePtr<uint8_t>(std::span{decodedPcm}), maxFrames, channels, &numFrames);

    if (status != 0)
    {
      return ao::makeError(ao::Error::Code::DecodeFailed, "ALAC decode failed");
    }

    auto block = PcmBlock{};
    decodedPcm.resize(static_cast<std::size_t>(numFrames) * targetBytesPerFrame);
    block.bytes = std::move(decodedPcm);
    block.frames = numFrames;
    block.bitDepth = targetBps;

    _impl->currentSampleIndex++;
    block.endOfStream = (_impl->currentSampleIndex >= _impl->demuxer->sampleCount());

    return block;
  }

  DecodedStreamInfo AlacDecoderSession::streamInfo() const
  {
    return _impl->info;
  }
} // namespace ao::audio
