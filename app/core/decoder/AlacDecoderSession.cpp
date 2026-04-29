// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/decoder/AlacDecoderSession.h"
#include "core/Log.h"

#include <alac/ALACAudioTypes.h>
#include <alac/ALACBitUtilities.h>
#include <alac/ALACDecoder.h>

#include <rs/media/mp4/Demuxer.h>
#include <rs/utility/MappedFile.h>

namespace app::core::decoder
{
  using namespace rs::media::mp4;

  namespace
  {
    std::uint32_t bytesPerSample(std::uint8_t bitDepth) noexcept
    {
      if (bitDepth == 24U)
      {
        return 3;
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
    AudioFormat requestedOutput;
    std::string error;
    DecodedStreamInfo info;

    std::unique_ptr<ALACDecoder> decoder;

    std::uint32_t currentSampleIndex = 0;
    std::uint32_t timescale = 0;

    rs::utility::MappedFile mappedFile;
    std::unique_ptr<Demuxer> demuxer;

    Impl(AudioFormat output)
      : requestedOutput(output)
    {
      decoder = std::make_unique<ALACDecoder>();
    }

    void setError(std::string_view msg) { error = std::string(msg); }
  };

  AlacDecoderSession::AlacDecoderSession(AudioFormat outputFormat)
    : _impl(std::make_unique<Impl>(outputFormat))
  {
  }

  AlacDecoderSession::~AlacDecoderSession() = default;

  bool AlacDecoderSession::open(std::filesystem::path const& filePath)
  {
    close();

    auto const mapError = _impl->mappedFile.map(filePath);

    if (!mapError.empty())
    {
      _impl->setError(mapError);
      return false;
    }

    _impl->demuxer = std::make_unique<Demuxer>(_impl->mappedFile.bytes());
    auto const demuxError = _impl->demuxer->parseTrack("alac");

    if (!demuxError.empty())
    {
      _impl->setError(demuxError);
      return false;
    }

    auto const cookie = _impl->demuxer->magicCookie();
    auto const initStatus =
      _impl->decoder->Init(const_cast<std::uint8_t*>(reinterpret_cast<std::uint8_t const*>(cookie.data())),
                           static_cast<std::uint32_t>(cookie.size()));

    if (initStatus != ALAC_noErr)
    {
      _impl->setError("Failed to initialize ALAC decoder");
      return false;
    }

    auto const& config = _impl->decoder->mConfig;

    if (config.sampleRate == 0 || config.numChannels == 0 || config.bitDepth == 0)
    {
      _impl->setError("Invalid ALAC stream configuration");
      return false;
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

    return true;
  }

  void AlacDecoderSession::close()
  {
    _impl->demuxer.reset();
    _impl->mappedFile.unmap();
    _impl->currentSampleIndex = 0;
    _impl->timescale = 0;
  }

  bool AlacDecoderSession::seek(std::uint32_t /*positionMs*/)
  {
    if (_impl->timescale == 0)
    {
      return false;
    }

    _impl->currentSampleIndex = 0;
    return true;
  }

  void AlacDecoderSession::flush()
  {
  }

  std::optional<PcmBlock> AlacDecoderSession::readNextBlock()
  {
    if (!_impl->demuxer || _impl->currentSampleIndex >= _impl->demuxer->sampleCount())
    {
      return PcmBlock{.bytes = {}, .endOfStream = true};
    }

    auto const packet = _impl->demuxer->getSamplePayload(_impl->currentSampleIndex);

    if (packet.empty())
    {
      _impl->setError("Failed to read ALAC sample payload");
      return std::nullopt;
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
      _impl->setError("Invalid ALAC format calculation");
      return std::nullopt;
    }

    std::uint32_t numFrames = 0;

    // If we need conversion (e.g. 24 -> 32), decode to temp buffer first
    if (sourceBps != targetBps)
    {
      std::vector<std::byte> sourcePcm(static_cast<std::size_t>(maxFrames) * sourceBytesPerFrame);
      auto bitBuffer = BitBuffer{};
      BitBufferInit(&bitBuffer,
                    const_cast<uint8_t*>(reinterpret_cast<uint8_t const*>(packet.data())),
                    static_cast<uint32_t>(packet.size()));

      auto const status = _impl->decoder->Decode(
        &bitBuffer, reinterpret_cast<uint8_t*>(sourcePcm.data()), maxFrames, channels, &numFrames);
      if (status != 0)
      {
        _impl->setError("ALAC decode failed");
        return std::nullopt;
      }

      std::vector<std::byte> targetPcm(static_cast<std::size_t>(numFrames) * targetBytesPerFrame);

      if (sourceBps == 24 && targetBps == 32)
      {
        auto* src = reinterpret_cast<std::uint8_t*>(sourcePcm.data());
        auto* dst = reinterpret_cast<std::int32_t*>(targetPcm.data());
        for (std::uint32_t i = 0; i < numFrames * channels; ++i)
        {
          std::int32_t val = src[0] | (src[1] << 8) | (src[2] << 16);
          if (val & 0x800000)
          {
            val |= 0xFF000000;
          }
          *dst++ = val;
          src += 3;
        }
      }
      else
      {
        _impl->setError(std::format("Unsupported ALAC conversion: {} -> {}", sourceBps, targetBps));
        return std::nullopt;
      }

      auto block = PcmBlock{};
      block.bytes = std::move(targetPcm);
      block.frames = numFrames;
      block.bitDepth = targetBps;
      _impl->currentSampleIndex++;
      block.endOfStream = (_impl->currentSampleIndex >= _impl->demuxer->sampleCount());
      return block;
    }
    else
    {
      // Direct decode
      std::vector<std::byte> decodedPcm(static_cast<std::size_t>(maxFrames) * targetBytesPerFrame);

      auto bitBuffer = BitBuffer{};
      BitBufferInit(&bitBuffer,
                    const_cast<uint8_t*>(reinterpret_cast<uint8_t const*>(packet.data())),
                    static_cast<uint32_t>(packet.size()));

      auto const status = _impl->decoder->Decode(
        &bitBuffer, reinterpret_cast<uint8_t*>(decodedPcm.data()), maxFrames, channels, &numFrames);

      if (status != 0)
      {
        _impl->setError("ALAC decode failed");
        return std::nullopt;
      }

      auto block = PcmBlock{};
      decodedPcm.resize(numFrames * targetBytesPerFrame);
      block.bytes = std::move(decodedPcm);
      block.frames = numFrames;
      block.bitDepth = targetBps;

      _impl->currentSampleIndex++;
      block.endOfStream = (_impl->currentSampleIndex >= _impl->demuxer->sampleCount());

      return block;
    }
  }

  DecodedStreamInfo AlacDecoderSession::streamInfo() const
  {
    return _impl->info;
  }

  std::string_view AlacDecoderSession::lastError() const noexcept
  {
    return _impl->error;
  }

} // namespace app::core::decoder
