// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "detail/OutputFormatValidation.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/Format.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/WavDecoderSession.h>
#include <ao/audio/detail/DecoderError.h>
#include <ao/media/wav/Riff.h>
#include <ao/utility/MappedFile.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace ao::audio
{
  namespace
  {
    constexpr std::uint32_t kMaxBlockFrames = 4096;
    constexpr std::uint8_t kLowByteMask = 0xFF;
    constexpr std::int32_t kUnsigned8Bias = 128;

    std::uint16_t readLe16(std::span<std::byte const> bytes, std::size_t offset) noexcept
    {
      return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes[offset])) |
             static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes[offset + 1]) << 8U);
    }

    std::int32_t readLe24(std::span<std::byte const> bytes, std::size_t offset) noexcept
    {
      static constexpr std::uint32_t kS24SignBit = 0x800000;
      static constexpr std::uint32_t kS24SignExtensionMask = 0xFF000000U;

      auto bits = static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(bytes[offset])) |
                  (static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(bytes[offset + 1])) << 8U) |
                  (static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(bytes[offset + 2])) << 16U);

      if ((bits & kS24SignBit) != 0)
      {
        bits |= kS24SignExtensionMask;
      }

      return std::bit_cast<std::int32_t>(bits);
    }

    std::uint32_t readLe32Bits(std::span<std::byte const> bytes, std::size_t offset) noexcept
    {
      return static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(bytes[offset])) |
             (static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(bytes[offset + 1])) << 8U) |
             (static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(bytes[offset + 2])) << 16U) |
             (static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(bytes[offset + 3])) << 24U);
    }

    std::int32_t readLe32(std::span<std::byte const> bytes, std::size_t offset) noexcept
    {
      return std::bit_cast<std::int32_t>(readLe32Bits(bytes, offset));
    }

    float readLeFloat32(std::span<std::byte const> bytes, std::size_t offset) noexcept
    {
      return std::bit_cast<float>(readLe32Bits(bytes, offset));
    }

    std::int32_t readIntegerSample(std::span<std::byte const> source, std::uint16_t bitsPerSample) noexcept
    {
      switch (bitsPerSample)
      {
        case 8:
        {
          auto const value = static_cast<std::int32_t>(std::to_integer<std::uint8_t>(source[0]));
          return value - kUnsigned8Bias;
        }

        case 16: return static_cast<std::int16_t>(readLe16(source, 0));
        case 24: return readLe24(source, 0);
        case 32: return readLe32(source, 0);
        default: return 0;
      }
    }

    std::int32_t alignSample(std::int32_t sample, std::uint8_t sourceBits, std::uint8_t outputBits) noexcept
    {
      if (sourceBits > outputBits)
      {
        return sample >> (sourceBits - outputBits);
      }

      if (sourceBits < outputBits)
      {
        auto bits = static_cast<std::uint32_t>(sample);
        bits <<= outputBits - sourceBits;
        return std::bit_cast<std::int32_t>(bits);
      }

      return sample;
    }

    void writeIntegerSample(std::span<std::byte> destination,
                            std::int32_t sample,
                            std::uint8_t sourceBits,
                            std::uint8_t outputBits) noexcept
    {
      auto const aligned = alignSample(sample, sourceBits, outputBits);

      if (outputBits == 16)
      {
        auto const value = static_cast<std::int16_t>(aligned);
        auto const bits = static_cast<std::uint16_t>(value);
        destination[0] = std::byte{static_cast<std::uint8_t>(bits & kLowByteMask)};
        destination[1] = std::byte{static_cast<std::uint8_t>((bits >> 8U) & kLowByteMask)};
      }
      else if (outputBits == 24)
      {
        auto const bits = static_cast<std::uint32_t>(aligned);
        destination[0] = std::byte{static_cast<std::uint8_t>(bits & kLowByteMask)};
        destination[1] = std::byte{static_cast<std::uint8_t>((bits >> 8U) & kLowByteMask)};
        destination[2] = std::byte{static_cast<std::uint8_t>((bits >> 16U) & kLowByteMask)};
      }
      else if (outputBits == 32)
      {
        auto const bits = static_cast<std::uint32_t>(aligned);
        destination[0] = std::byte{static_cast<std::uint8_t>(bits & kLowByteMask)};
        destination[1] = std::byte{static_cast<std::uint8_t>((bits >> 8U) & kLowByteMask)};
        destination[2] = std::byte{static_cast<std::uint8_t>((bits >> 16U) & kLowByteMask)};
        destination[3] = std::byte{static_cast<std::uint8_t>((bits >> 24U) & kLowByteMask)};
      }
    }

    bool canCopyIntegerPcmBytes(Format const& source, Format const& output) noexcept
    {
      // RIFF PCM input and this decoder's integer PcmBlock output are both
      // little-endian. The sample type, interleaving, container width, and
      // valid width must also match before their byte representations are
      // identical. The parser currently rejects restricted-valid-width input;
      // keeping that requirement here makes the fast path independently safe.
      return !source.isFloat && !output.isFloat && source.isInterleaved && output.isInterleaved &&
             source.bitDepth == output.bitDepth && source.validBits == source.bitDepth &&
             output.validBits == output.bitDepth;
    }

    bool isSupportedIntegerOutput(std::uint8_t bitDepth) noexcept
    {
      return bitDepth == 16 || bitDepth == 24 || bitDepth == 32;
    }

    Format selectFloatOutputFormat(Format const& requestedOutput, Format sourceFormat)
    {
      auto outputFormat = sourceFormat;

      if (requestedOutput.bitDepth == 0)
      {
        if (requestedOutput.validBits != 0)
        {
          detail::throwDecoderError(Error::Code::NotSupported, "Unsupported WAV float output format");
        }

        outputFormat.bitDepth = 32;
        outputFormat.validBits = 32;
        outputFormat.isFloat = true;
        return outputFormat;
      }

      if (requestedOutput.isFloat)
      {
        if (requestedOutput.bitDepth != 32 || (requestedOutput.validBits != 0 && requestedOutput.validBits != 32))
        {
          detail::throwDecoderError(Error::Code::NotSupported, "Unsupported WAV float output format");
        }

        outputFormat.bitDepth = 32;
        outputFormat.validBits = 32;
        outputFormat.isFloat = true;
        return outputFormat;
      }

      auto const outputValidBits =
        requestedOutput.validBits != 0 ? requestedOutput.validBits : requestedOutput.bitDepth;

      if (!isSupportedIntegerOutput(requestedOutput.bitDepth) || outputValidBits != requestedOutput.bitDepth)
      {
        detail::throwDecoderError(Error::Code::NotSupported, "Unsupported WAV float output format");
      }

      outputFormat.bitDepth = requestedOutput.bitDepth;
      outputFormat.validBits = outputValidBits;
      outputFormat.isFloat = false;
      return outputFormat;
    }

    Format selectIntegerOutputFormat(Format const& requestedOutput, Format sourceFormat)
    {
      auto outputFormat = sourceFormat;
      auto outputBitDepth = requestedOutput.bitDepth;

      if (outputBitDepth == 0)
      {
        outputBitDepth = sourceFormat.bitDepth <= 8 ? 16 : sourceFormat.bitDepth;
      }

      if (requestedOutput.isFloat || !isSupportedIntegerOutput(outputBitDepth))
      {
        detail::throwDecoderError(Error::Code::NotSupported, "Unsupported WAV integer output format");
      }

      auto const outputValidBits =
        requestedOutput.validBits != 0 ? requestedOutput.validBits : std::min(sourceFormat.validBits, outputBitDepth);

      if (outputValidBits != std::min(sourceFormat.validBits, outputBitDepth))
      {
        detail::throwDecoderError(Error::Code::NotSupported, "Unsupported WAV output valid bits");
      }

      outputFormat.bitDepth = outputBitDepth;
      outputFormat.validBits = outputValidBits;
      outputFormat.isFloat = false;
      return outputFormat;
    }

    std::int32_t floatToIntegerSample(float sample, std::uint8_t outputBits) noexcept
    {
      if (std::isnan(sample))
      {
        return 0;
      }

      auto const minValue = -(std::int64_t{1} << (outputBits - 1U));
      auto const maxValue = (std::int64_t{1} << (outputBits - 1U)) - 1;

      if (sample <= -1.0F)
      {
        return static_cast<std::int32_t>(minValue);
      }

      if (sample >= 1.0F)
      {
        return static_cast<std::int32_t>(maxValue);
      }

      if (!std::isfinite(sample))
      {
        return 0;
      }

      auto const scaled = std::llround(static_cast<double>(sample) * static_cast<double>(maxValue));
      return static_cast<std::int32_t>(std::clamp<std::int64_t>(scaled, minValue, maxValue));
    }
  } // namespace

  struct WavDecoderSession::Impl final
  {
    Format requestedOutput;
    utility::MappedFile file;
    DecodedStreamInfo info;
    std::vector<std::byte> pcmBuffer{};
    std::uint64_t nextFrameIndex = 0;
    std::uint64_t totalFrames = 0;
    std::size_t dataOffset = 0;
    std::size_t dataSize = 0;
    std::uint16_t sourceBitsPerSample = 0;
    std::uint16_t sourceBlockAlign = 0;
    bool eof = false;

    explicit Impl(Format output)
      : requestedOutput{output}
    {
    }

    void selectOutputFormat(media::wav::FormatChunk const& format)
    {
      auto sourceFormat = Format{
        .sampleRate = format.sampleRate,
        .channels = static_cast<std::uint8_t>(format.channels),
        .bitDepth = static_cast<std::uint8_t>(format.bitsPerSample),
        .validBits = static_cast<std::uint8_t>(format.validBitsPerSample),
        .isFloat = format.isFloat,
        .isInterleaved = true,
      };

      auto outputFormat = format.isFloat ? selectFloatOutputFormat(requestedOutput, sourceFormat)
                                         : selectIntegerOutputFormat(requestedOutput, sourceFormat);

      if (auto const result = detail::validateFixedOutputRequest(requestedOutput, outputFormat, "WAV"); !result)
      {
        detail::throwDecoderError(result.error());
      }

      info.sourceFormat = sourceFormat;
      info.outputFormat = outputFormat;
      info.isLossy = false;
      info.codec = AudioCodec::Wav;
    }

    std::span<std::byte const> dataBytes() const noexcept { return file.bytes().subspan(dataOffset, dataSize); }
  };

  WavDecoderSession::WavDecoderSession(Format outputFormat)
    : _implPtr{std::make_unique<Impl>(outputFormat)}
  {
  }

  WavDecoderSession::~WavDecoderSession() = default;

  Result<> WavDecoderSession::openCodec(std::filesystem::path const& filePath)
  {
    try
    {
      close();

      if (auto const result = _implPtr->file.map(filePath); !result)
      {
        detail::throwDecoderError(result.error());
      }

      auto parsedResult = media::wav::parseWave(_implPtr->file.bytes(), media::wav::WaveParseExtent::RequiredAudio);

      if (!parsedResult)
      {
        detail::throwDecoderError(parsedResult.error());
      }

      auto const& parsed = *parsedResult;
      _implPtr->selectOutputFormat(parsed.format);
      _implPtr->dataOffset = parsed.dataOffset;
      _implPtr->dataSize = parsed.data.size();
      _implPtr->sourceBitsPerSample = parsed.format.bitsPerSample;
      _implPtr->sourceBlockAlign = parsed.format.blockAlign;
      _implPtr->totalFrames = parsed.data.size() / parsed.format.blockAlign;
      _implPtr->nextFrameIndex = 0;
      _implPtr->eof = false;

      _implPtr->info.duration = samplesToDuration(_implPtr->totalFrames, parsed.format.sampleRate);
      return {};
    }
    catch (detail::DecoderException const& ex)
    {
      close();
      return std::unexpected{ex.error()};
    }
  }

  void WavDecoderSession::close() noexcept
  {
    _implPtr->file.unmap();
    _implPtr->info = {};
    _implPtr->pcmBuffer.clear();
    _implPtr->nextFrameIndex = 0;
    _implPtr->totalFrames = 0;
    _implPtr->dataOffset = 0;
    _implPtr->dataSize = 0;
    _implPtr->sourceBitsPerSample = 0;
    _implPtr->sourceBlockAlign = 0;
    _implPtr->eof = false;
  }

  // Result error materialization may allocate; DecoderSession intentionally
  // fails fast if an allocation escapes this noexcept boundary.
  Result<> WavDecoderSession::seek(std::chrono::milliseconds offset) noexcept
  {
    if (!_implPtr->file.isMapped())
    {
      return makeError(Error::Code::SeekFailed, "WAV decoder is not open");
    }

    if (offset > _implPtr->info.duration)
    {
      return makeError(Error::Code::SeekFailed, "Seek offset out of bounds");
    }

    auto const frameIndex = durationToSamples(offset, _implPtr->info.sourceFormat.sampleRate);

    if (frameIndex > _implPtr->totalFrames)
    {
      return makeError(Error::Code::SeekFailed, "Seek offset out of bounds");
    }

    _implPtr->nextFrameIndex = frameIndex;
    _implPtr->eof = frameIndex == _implPtr->totalFrames;
    _implPtr->pcmBuffer.clear();
    return {};
  }

  void WavDecoderSession::flush() noexcept
  {
    _implPtr->pcmBuffer.clear();
  }

  // The decode buffer may allocate; DecoderSession intentionally fails fast if
  // an allocation escapes this noexcept boundary.
  Result<PcmBlock> WavDecoderSession::readNextBlock() noexcept
  {
    if (!_implPtr->file.isMapped() || _implPtr->eof)
    {
      return PcmBlock{.endOfStream = true};
    }

    auto const remainingFrames = _implPtr->totalFrames - _implPtr->nextFrameIndex;
    auto const frames =
      static_cast<std::uint32_t>(std::min<std::uint64_t>(remainingFrames, static_cast<std::uint64_t>(kMaxBlockFrames)));
    auto const byteOffset = static_cast<std::size_t>(_implPtr->nextFrameIndex) * _implPtr->sourceBlockAlign;
    auto const sourceByteCount = static_cast<std::size_t>(frames) * _implPtr->sourceBlockAlign;
    auto const sourceBytes = _implPtr->dataBytes().subspan(byteOffset, sourceByteCount);
    auto const currentFrameIndex = _implPtr->nextFrameIndex;

    _implPtr->pcmBuffer.clear();

    if (_implPtr->info.outputFormat.isFloat ||
        canCopyIntegerPcmBytes(_implPtr->info.sourceFormat, _implPtr->info.outputFormat))
    {
      _implPtr->pcmBuffer.resize(sourceBytes.size());
      std::memcpy(_implPtr->pcmBuffer.data(), sourceBytes.data(), sourceBytes.size());
    }
    else if (_implPtr->info.sourceFormat.isFloat)
    {
      auto const outputSampleBytes = static_cast<std::size_t>(bytesPerSample(_implPtr->info.outputFormat));
      auto const sampleCount = static_cast<std::size_t>(frames) * _implPtr->info.outputFormat.channels;

      _implPtr->pcmBuffer.resize(sampleCount * outputSampleBytes);
      auto outputBytes = std::span<std::byte>{_implPtr->pcmBuffer};

      for (std::size_t sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
      {
        auto const sampleOffset = sampleIndex * sizeof(float);
        auto const sample = floatToIntegerSample(
          readLeFloat32(sourceBytes.subspan(sampleOffset, sizeof(float)), 0), _implPtr->info.outputFormat.bitDepth);
        writeIntegerSample(outputBytes.subspan(sampleIndex * outputSampleBytes, outputSampleBytes),
                           sample,
                           _implPtr->info.outputFormat.bitDepth,
                           _implPtr->info.outputFormat.bitDepth);
      }
    }
    else
    {
      auto const sourceSampleBytes = static_cast<std::size_t>((_implPtr->sourceBitsPerSample + 7U) / 8U);
      auto const outputSampleBytes = static_cast<std::size_t>(bytesPerSample(_implPtr->info.outputFormat));
      auto const sampleCount = static_cast<std::size_t>(frames) * _implPtr->info.outputFormat.channels;

      _implPtr->pcmBuffer.resize(sampleCount * outputSampleBytes);
      auto outputBytes = std::span<std::byte>{_implPtr->pcmBuffer};

      for (std::size_t sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
      {
        auto const sampleOffset = sampleIndex * sourceSampleBytes;
        auto const sample =
          readIntegerSample(sourceBytes.subspan(sampleOffset, sourceSampleBytes), _implPtr->sourceBitsPerSample);
        writeIntegerSample(outputBytes.subspan(sampleIndex * outputSampleBytes, outputSampleBytes),
                           sample,
                           _implPtr->info.sourceFormat.validBits,
                           _implPtr->info.outputFormat.bitDepth);
      }
    }

    _implPtr->nextFrameIndex += frames;
    _implPtr->eof = _implPtr->nextFrameIndex == _implPtr->totalFrames;

    return PcmBlock{.bytes = _implPtr->pcmBuffer,
                    .bitDepth = _implPtr->info.outputFormat.bitDepth,
                    .frames = frames,
                    .firstFrameIndex = currentFrameIndex,
                    .endOfStream = _implPtr->eof};
  }

  DecodedStreamInfo WavDecoderSession::streamInfo() const noexcept
  {
    return _implPtr->info;
  }
} // namespace ao::audio
