// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "detail/Mp4PacketSource.h"
#include "detail/OutputFormatValidation.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/audio/AlacDecoderSession.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/Format.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/PcmConversion.h>
#include <ao/audio/detail/DecoderError.h>
#include <ao/utility/ByteView.h>

#include <alac/ALACAudioTypes.h>
#include <alac/ALACBitUtilities.h>
#include <alac/ALACDecoder.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio
{
  using namespace utility;

  namespace
  {
    [[maybe_unused]] constexpr std::int32_t kSignExtensionMask = ~0x00FFFFFF;
    constexpr std::size_t kAlacConfigOffset = 12;
    constexpr std::size_t kAlacConfigSize = 24;
    constexpr std::size_t kAlacCompatibleVersionOffset = kAlacConfigOffset + 4;
    constexpr std::size_t kAlacBitDepthOffset = kAlacConfigOffset + 5;
    constexpr std::size_t kAlacChannelCountOffset = kAlacConfigOffset + 9;
    constexpr std::size_t kAlacSampleRateOffset = kAlacConfigOffset + 20;
    constexpr std::uint32_t kMaxAlacFrameLength = 16384;
    constexpr std::uint32_t kMaxAlacSampleRate = 384000;
    constexpr std::uint8_t kMaxAlacChannels = 8;
    constexpr std::size_t kBigEndian32Byte1Offset = 1;
    constexpr std::size_t kBigEndian32Byte2Offset = 2;
    constexpr std::size_t kBigEndian32Byte3Offset = 3;

    std::uint32_t readBigEndian32(std::span<std::byte const> bytes, std::size_t offset) noexcept
    {
      return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
             (static_cast<std::uint32_t>(bytes[offset + kBigEndian32Byte1Offset]) << 16U) |
             (static_cast<std::uint32_t>(bytes[offset + kBigEndian32Byte2Offset]) << 8U) |
             static_cast<std::uint32_t>(bytes[offset + kBigEndian32Byte3Offset]);
    }

    void validateAlacCookie(std::span<std::byte const> cookie)
    {
      if (cookie.size() < kAlacConfigOffset + kAlacConfigSize ||
          utility::bytes::stringView(cookie.subspan(4, 4)) != "alac")
      {
        detail::throwDecoderError(Error::Code::InitFailed, "Malformed ALAC configuration");
      }

      auto const frameLength = readBigEndian32(cookie, kAlacConfigOffset);
      auto const compatibleVersion = static_cast<std::uint8_t>(cookie[kAlacCompatibleVersionOffset]);
      auto const bitDepth = static_cast<std::uint8_t>(cookie[kAlacBitDepthOffset]);
      auto const channels = static_cast<std::uint8_t>(cookie[kAlacChannelCountOffset]);
      auto const sampleRate = readBigEndian32(cookie, kAlacSampleRateOffset);
      auto const supportedBitDepth = bitDepth == 16 || bitDepth == 20 || bitDepth == 24 || bitDepth == 32;

      if (frameLength == 0 || frameLength > kMaxAlacFrameLength || compatibleVersion != 0 || !supportedBitDepth ||
          channels == 0 || channels > kMaxAlacChannels || sampleRate == 0 || sampleRate > kMaxAlacSampleRate)
      {
        detail::throwDecoderError(Error::Code::InitFailed, "Invalid ALAC stream configuration");
      }
    }
  } // namespace

  struct AlacDecoderSession::Impl final
  {
    Format requestedOutput;
    DecodedStreamInfo info;

    std::unique_ptr<ALACDecoder> decoderPtr;

    detail::Mp4PacketSource packetSource;

    std::vector<std::byte> sourcePcm;
    std::vector<std::byte> targetPcm;

    Impl(Format const& output)
      : requestedOutput{output}
    {
      decoderPtr = std::make_unique<ALACDecoder>();
    }

    std::uint32_t fallbackFrameLength() const noexcept
    {
      return decoderPtr->mConfig.frameLength > 0 ? decoderPtr->mConfig.frameLength
                                                 : static_cast<std::uint32_t>(kALACDefaultFramesPerPacket);
    }
  };

  AlacDecoderSession::AlacDecoderSession(Format outputFormat)
    : _implPtr{std::make_unique<Impl>(outputFormat)}
  {
  }

  AlacDecoderSession::~AlacDecoderSession() = default;

  Result<> AlacDecoderSession::openCodec(std::filesystem::path const& filePath)
  {
    try
    {
      if (auto const result = _implPtr->packetSource.open(filePath, "alac"); !result)
      {
        auto error = result.error();

        if (error.code == Error::Code::FormatRejected)
        {
          error.code = Error::Code::InitFailed;
        }

        detail::throwDecoderError(std::move(error));
      }

      auto const cookie = _implPtr->packetSource.magicCookie();

      validateAlacCookie(cookie);

      auto const initStatus =
        _implPtr->decoderPtr->Init(layout::asLegacyPtr<std::uint8_t>(cookie), layout::size32(cookie));

      if (initStatus != ALAC_noErr)
      {
        detail::throwDecoderError(Error::Code::InitFailed, "Failed to initialize ALAC decoder");
      }

      auto const& config = _implPtr->decoderPtr->mConfig;

      if (config.sampleRate == 0 || config.numChannels == 0 || config.bitDepth == 0)
      {
        detail::throwDecoderError(Error::Code::InitFailed, "Invalid ALAC stream configuration");
      }

      _implPtr->info.duration = _implPtr->packetSource.duration(config.sampleRate);
      _implPtr->info.codec = AudioCodec::Alac;

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
                                                  : _implPtr->info.sourceFormat.validBits;
      }

      if (auto const result =
            detail::validateFixedOutputRequest(_implPtr->requestedOutput, _implPtr->info.outputFormat, "ALAC");
          !result)
      {
        detail::throwDecoderError(result.error());
      }

      auto const sourceBitDepth = _implPtr->info.sourceFormat.bitDepth;
      auto const outputBitDepth = _implPtr->info.outputFormat.bitDepth;
      auto const supportedConversion = sourceBitDepth == outputBitDepth ||
                                       (sourceBitDepth == 16 && outputBitDepth == 32) ||
                                       (sourceBitDepth == 24 && outputBitDepth == 32);

      if (_implPtr->requestedOutput.isFloat || !supportedConversion ||
          _implPtr->info.outputFormat.validBits != sourceBitDepth)
      {
        detail::throwDecoderError(Error::Code::NotSupported,
                                  std::format("Unsupported ALAC conversion: {} -> {}", sourceBitDepth, outputBitDepth));
      }

      return {};
    }
    catch (detail::DecoderException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  void AlacDecoderSession::close() noexcept
  {
    _implPtr->packetSource.close();
    _implPtr->sourcePcm.clear();
    _implPtr->targetPcm.clear();
    _implPtr->info = {};
  }

  Result<> AlacDecoderSession::seek(std::chrono::milliseconds offset) noexcept
  {
    return _implPtr->packetSource.seek(offset, _implPtr->info.sourceFormat.sampleRate);
  }

  void AlacDecoderSession::flush() noexcept
  {
  }

  Result<PcmBlock> AlacDecoderSession::readNextBlock() noexcept
  {
    if (_implPtr->packetSource.isAtEnd())
    {
      return PcmBlock{.bytes = {}, .endOfStream = true};
    }

    auto const firstFrameIndex =
      _implPtr->packetSource.firstFrameIndex(_implPtr->info.sourceFormat.sampleRate, _implPtr->fallbackFrameLength());
    auto const packet = _implPtr->packetSource.packet();

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

    auto const sourceBytesPerFrame = channels * bytesPerSample(_implPtr->info.sourceFormat);
    auto const targetBytesPerFrame = channels * bytesPerSample(_implPtr->info.outputFormat);

    if (sourceBytesPerFrame == 0 || targetBytesPerFrame == 0)
    {
      return makeError(Error::Code::DecodeFailed, "Invalid ALAC format calculation");
    }

    std::uint32_t frameCount = 0;

    // Reuse member buffers to avoid per-block allocations
    if (sourceBps != targetBps)
    {
      _implPtr->sourcePcm.resize(static_cast<std::size_t>(maxFrames) * sourceBytesPerFrame);
      auto bitBuffer = BitBuffer{};
      ::BitBufferInit(&bitBuffer, layout::asLegacyPtr<std::uint8_t>(packet), layout::size32(packet));

      auto const status = _implPtr->decoderPtr->Decode(
        &bitBuffer, layout::asMutablePtr<uint8_t>(std::span{_implPtr->sourcePcm}), maxFrames, channels, &frameCount);

      if (status != 0)
      {
        return makeError(Error::Code::DecodeFailed, "ALAC decode failed");
      }

      if (frameCount == 0 || frameCount > maxFrames)
      {
        return makeError(Error::Code::DecodeFailed, "Invalid ALAC decoded frame count");
      }

      _implPtr->targetPcm.resize(static_cast<std::size_t>(frameCount) * targetBytesPerFrame);

      if (sourceBps == 16 && targetBps == 32)
      {
        auto const src = layout::viewArray<std::int16_t>(std::span{_implPtr->sourcePcm});
        auto const dst = layout::viewArrayMutable<std::int32_t>(std::span{_implPtr->targetPcm});
        padPcmSamples<std::int16_t, std::int32_t>(src, dst, 16);
      }
      else if (sourceBps == 24 && targetBps == 32)
      {
        auto const dst = layout::viewArrayMutable<std::int32_t>(std::span{_implPtr->targetPcm});
        unpackS24PcmSamples(_implPtr->sourcePcm, dst, 8);
      }
      else
      {
        return makeError(
          Error::Code::NotSupported, std::format("Unsupported ALAC conversion: {} -> {}", sourceBps, targetBps));
      }

      _implPtr->packetSource.advance();

      return PcmBlock{
        .bytes = _implPtr->targetPcm,
        .bitDepth = targetBps,
        .frames = frameCount,
        .firstFrameIndex = firstFrameIndex,
        .endOfStream = _implPtr->packetSource.isAtEnd(),
      };
    }

    // Direct decode
    _implPtr->targetPcm.resize(static_cast<std::size_t>(maxFrames) * targetBytesPerFrame);

    auto bitBuffer = BitBuffer{};
    ::BitBufferInit(&bitBuffer, layout::asLegacyPtr<std::uint8_t>(packet), layout::size32(packet));

    auto const status = _implPtr->decoderPtr->Decode(
      &bitBuffer, layout::asMutablePtr<uint8_t>(std::span{_implPtr->targetPcm}), maxFrames, channels, &frameCount);

    if (status != 0)
    {
      return makeError(Error::Code::DecodeFailed, "ALAC decode failed");
    }

    if (frameCount == 0 || frameCount > maxFrames)
    {
      return makeError(Error::Code::DecodeFailed, "Invalid ALAC decoded frame count");
    }

    _implPtr->targetPcm.resize(static_cast<std::size_t>(frameCount) * targetBytesPerFrame);
    _implPtr->packetSource.advance();

    return PcmBlock{
      .bytes = _implPtr->targetPcm,
      .bitDepth = targetBps,
      .frames = frameCount,
      .firstFrameIndex = firstFrameIndex,
      .endOfStream = _implPtr->packetSource.isAtEnd(),
    };
  }

  DecodedStreamInfo AlacDecoderSession::streamInfo() const noexcept
  {
    return _implPtr->info;
  }
} // namespace ao::audio
