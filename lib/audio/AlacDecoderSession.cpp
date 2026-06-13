// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "detail/Mp4PacketSource.h"
#include "detail/OutputFormatValidation.h"
#include <ao/Error.h>
#include <ao/audio/AlacDecoderSession.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Format.h>
#include <ao/audio/PcmConverter.h>
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
    constexpr std::uint8_t kBytesPer24BitSample = 3;
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

    std::uint32_t readBigEndian32(std::span<std::byte const> bytes, std::size_t offset) noexcept
    {
      return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
             (static_cast<std::uint32_t>(bytes[offset + kBigEndian32Byte1Offset]) << 16U) |
             (static_cast<std::uint32_t>(bytes[offset + kBigEndian32Byte2Offset]) << 8U) |
             static_cast<std::uint32_t>(bytes[offset + kBigEndian32Byte3Offset]);
    }

    Result<> validateAlacCookie(std::span<std::byte const> cookie)
    {
      if (cookie.size() < kAlacConfigOffset + kAlacConfigSize ||
          utility::bytes::stringView(cookie.subspan(4, 4)) != "alac")
      {
        return makeError(Error::Code::InitFailed, "Malformed ALAC configuration");
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
        return makeError(Error::Code::InitFailed, "Invalid ALAC stream configuration");
      }

      return {};
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

  Result<> AlacDecoderSession::open(std::filesystem::path const& filePath)
  {
    close();

    auto failOpen = [this](Error error) -> Result<>
    {
      close();
      return std::unexpected{std::move(error)};
    };

    if (auto const result = _implPtr->packetSource.open(filePath, "alac"); !result)
    {
      auto error = result.error();

      if (error.code == Error::Code::FormatRejected)
      {
        error.code = Error::Code::InitFailed;
      }

      return failOpen(std::move(error));
    }

    auto const cookie = _implPtr->packetSource.magicCookie();

    if (auto const result = validateAlacCookie(cookie); !result)
    {
      return failOpen(result.error());
    }

    auto const initStatus =
      _implPtr->decoderPtr->Init(layout::asLegacyPtr<std::uint8_t>(cookie), layout::size32(cookie));

    if (initStatus != ALAC_noErr)
    {
      return failOpen(Error{.code = Error::Code::InitFailed, .message = "Failed to initialize ALAC decoder"});
    }

    auto const& config = _implPtr->decoderPtr->mConfig;

    if (config.sampleRate == 0 || config.numChannels == 0 || config.bitDepth == 0)
    {
      return failOpen(Error{.code = Error::Code::InitFailed, .message = "Invalid ALAC stream configuration"});
    }

    _implPtr->info.duration = _implPtr->packetSource.duration(config.sampleRate);

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
      return failOpen(result.error());
    }

    auto const sourceBitDepth = _implPtr->info.sourceFormat.bitDepth;
    auto const outputBitDepth = _implPtr->info.outputFormat.bitDepth;
    auto const supportedConversion = sourceBitDepth == outputBitDepth ||
                                     (sourceBitDepth == 16 && outputBitDepth == 32) ||
                                     (sourceBitDepth == 24 && outputBitDepth == 32);

    if (_implPtr->requestedOutput.isFloat || !supportedConversion ||
        _implPtr->info.outputFormat.validBits != sourceBitDepth)
    {
      return failOpen(
        Error{.code = Error::Code::NotSupported,
              .message = std::format("Unsupported ALAC conversion: {} -> {}", sourceBitDepth, outputBitDepth)});
    }

    return {};
  }

  void AlacDecoderSession::close()
  {
    _implPtr->packetSource.close();
    _implPtr->sourcePcm.clear();
    _implPtr->targetPcm.clear();
    _implPtr->info = {};
  }

  Result<> AlacDecoderSession::seek(std::chrono::milliseconds offset)
  {
    return _implPtr->packetSource.seek(offset, _implPtr->info.sourceFormat.sampleRate);
  }

  void AlacDecoderSession::flush()
  {
  }

  Result<PcmBlock> AlacDecoderSession::readNextBlock()
  {
    if (_implPtr->packetSource.atEnd())
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

      if (numFrames == 0 || numFrames > maxFrames)
      {
        return makeError(Error::Code::DecodeFailed, "Invalid ALAC decoded frame count");
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

      _implPtr->packetSource.advance();

      return PcmBlock{
        .bytes = _implPtr->targetPcm,
        .bitDepth = targetBps,
        .frames = numFrames,
        .firstFrameIndex = firstFrameIndex,
        .endOfStream = _implPtr->packetSource.atEnd(),
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

    if (numFrames == 0 || numFrames > maxFrames)
    {
      return makeError(Error::Code::DecodeFailed, "Invalid ALAC decoded frame count");
    }

    _implPtr->targetPcm.resize(static_cast<std::size_t>(numFrames) * targetBytesPerFrame);
    _implPtr->packetSource.advance();

    return PcmBlock{
      .bytes = _implPtr->targetPcm,
      .bitDepth = targetBps,
      .frames = numFrames,
      .firstFrameIndex = firstFrameIndex,
      .endOfStream = _implPtr->packetSource.atEnd(),
    };
  }

  DecodedStreamInfo AlacDecoderSession::streamInfo() const
  {
    return _implPtr->info;
  }
} // namespace ao::audio
