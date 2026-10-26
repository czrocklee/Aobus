// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "WavDecoderSession.h"

#include "AudioTime.h"
#include "detail/DecoderError.h"
#include "detail/DecoderOutputAdapter.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/media/wav/Riff.h>
#include <ao/utility/MappedFile.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
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

    struct WavChannelLayout final
    {
      std::uint32_t effectiveMask;
      ChannelLayoutEvidence evidence;
    };

    Result<WavChannelLayout> wavChannelLayout(media::wav::FormatChunk const& format)
    {
      // These are WAV compatibility identities, not count-derived native maps.
      static constexpr auto kCompatibilityMasks =
        std::to_array<std::uint32_t>({0x4, 0x3, 0x7, 0x33, 0x37, 0x3F, 0x70F, 0x63F});
      static constexpr std::uint32_t kAllSpeakers = 0x80000000;
      auto const optDeclaration = format.optChannelMask;

      if (!optDeclaration || *optDeclaration == 0)
      {
        if (format.channels >= 1 && format.channels <= kCompatibilityMasks.size())
        {
          // Absent mono/stereo declarations identify format defaults; explicit
          // zero and wider absent counts use inferred compatibility identities.
          auto const origin =
            !optDeclaration && format.channels <= 2 ? ChannelLayoutOrigin::Identified : ChannelLayoutOrigin::Inferred;
          return WavChannelLayout{
            .effectiveMask = kCompatibilityMasks[format.channels - 1U],
            .evidence = ChannelLayoutEvidence{.origin = origin},
          };
        }

        return makeError(Error::Code::NotSupported,
                         std::format("WAV channel layout: {} channels with {} has no supported fallback",
                                     format.channels,
                                     optDeclaration ? "explicit mask 0x00000000" : "no mask declaration"));
      }

      auto const declaration = *optDeclaration;

      // ALL is a wildcard policy flag, not an excess assigned position.
      if ((declaration & kAllSpeakers) != 0)
      {
        return makeError(
          Error::Code::NotSupported,
          std::format(
            "WAV channel layout: mask 0x{:08X} for {} channels contains a wildcard", declaration, format.channels));
      }

      if (std::popcount(declaration) < format.channels)
      {
        return makeError(
          Error::Code::NotSupported,
          std::format(
            "WAV channel layout: mask 0x{:08X} only partially assigns {} channels", declaration, format.channels));
      }

      auto effectiveMask = declaration;

      // WAVE ignores excess highest positions. Count still owns PCM stride,
      // and only the effective interpretation is published.
      while (std::popcount(effectiveMask) > format.channels)
      {
        effectiveMask &= ~std::bit_floor(effectiveMask);
      }

      if (!isCompleteWaveSpeakerMask(effectiveMask, format.channels))
      {
        return makeError(Error::Code::NotSupported,
                         std::format("WAV channel layout: mask 0x{:08X} for {} channels activates reserved positions "
                                     "(effective mask 0x{:08X})",
                                     declaration,
                                     format.channels,
                                     effectiveMask));
      }

      return WavChannelLayout{
        .effectiveMask = effectiveMask,
        .evidence = ChannelLayoutEvidence{.origin = ChannelLayoutOrigin::Identified},
      };
    }

    SampleEncoding nativeWavEncoding(media::wav::FormatChunk const& format)
    {
      if (format.isFloat)
      {
        return format.bitsPerSample == 32U ? SampleEncoding::Float32Le : SampleEncoding::Unknown;
      }

      if (format.bitsPerSample <= 16U)
      {
        return SampleEncoding::Signed16Le;
      }

      if (format.bitsPerSample <= 24U)
      {
        return SampleEncoding::Signed24PackedLe;
      }

      if (format.bitsPerSample <= 32U)
      {
        return SampleEncoding::Signed32Le;
      }

      return SampleEncoding::Unknown;
    }
  } // namespace

  struct WavDecoderSession::Impl final
  {
    detail::DecoderOutputAdapter outputAdapter;
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

    explicit Impl(std::optional<SampleEncoding> optOutputEncoding)
      : outputAdapter{optOutputEncoding}
    {
    }

    void selectOutputFormat(media::wav::FormatChunk const& format)
    {
      auto const layoutRes = wavChannelLayout(format);

      if (!layoutRes)
      {
        detail::throwDecoderError(layoutRes.error());
      }

      auto const sourceFormat = SignalFormat{
        .sampleRate = format.sampleRate,
        .channels = static_cast<std::uint8_t>(format.channels),
        .precisionBits = static_cast<std::uint8_t>(format.validBitsPerSample),
        .sampleKind = format.isFloat ? SampleKind::FloatingPoint : SampleKind::Integer,
        .speakerMask = layoutRes->effectiveMask,
      };
      auto const configuredRes = outputAdapter.configure(sourceFormat, nativeWavEncoding(format));

      if (!configuredRes)
      {
        detail::throwDecoderError(configuredRes.error());
      }

      info.sourceFormat = sourceFormat;
      info.outputFormat = *configuredRes;
      info.sourceChannelLayout = layoutRes->evidence;
      info.isLossy = false;
      info.codec = AudioCodec::Wav;
    }

    std::span<std::byte const> dataBytes() const noexcept { return file.bytes().subspan(dataOffset, dataSize); }
  };

  WavDecoderSession::WavDecoderSession(std::optional<SampleEncoding> optOutputEncoding)
    : _implPtr{std::make_unique<Impl>(optOutputEncoding)}
  {
  }

  WavDecoderSession::~WavDecoderSession() = default;

  Result<> WavDecoderSession::initialize(std::filesystem::path const& filePath) noexcept
  {
    try
    {
      if (auto const res = _implPtr->file.map(filePath); !res)
      {
        detail::throwDecoderError(res.error());
      }

      auto parsedRes = media::wav::parseWave(_implPtr->file.bytes(), media::wav::WaveParseExtent::RequiredAudio);

      if (!parsedRes)
      {
        detail::throwDecoderError(parsedRes.error());
      }

      auto const& parsed = *parsedRes;
      _implPtr->selectOutputFormat(parsed.format);
      _implPtr->dataOffset = parsed.dataOffset;
      _implPtr->dataSize = parsed.data.size();
      _implPtr->sourceBitsPerSample = parsed.format.bitsPerSample;
      _implPtr->sourceBlockAlign = parsed.format.blockAlign;
      _implPtr->totalFrames = parsed.data.size() / parsed.format.blockAlign;
      _implPtr->info.optTotalFrames = _implPtr->totalFrames;
      _implPtr->nextFrameIndex = 0;
      _implPtr->eof = false;

      _implPtr->info.duration = samplesToDuration(_implPtr->totalFrames, parsed.format.sampleRate);
      return {};
    }
    catch (detail::DecoderException const& ex)
    {
      return std::unexpected{ex.error()};
    }
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

    return seekFrame(durationToSamples(offset, _implPtr->info.sourceFormat.sampleRate));
  }

  Result<> WavDecoderSession::seekFrame(std::uint64_t frameIndex) noexcept
  {
    if (!_implPtr->file.isMapped())
    {
      return makeError(Error::Code::SeekFailed, "WAV decoder is not open");
    }

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

    if (_implPtr->info.sourceFormat.sampleKind == SampleKind::FloatingPoint)
    {
      _implPtr->pcmBuffer.resize(sourceBytes.size());
      std::memcpy(_implPtr->pcmBuffer.data(), sourceBytes.data(), sourceBytes.size());
    }
    else
    {
      auto const sourceSampleBytes = static_cast<std::size_t>((_implPtr->sourceBitsPerSample + 7U) / 8U);
      auto const nativeEncoding = _implPtr->outputAdapter.nativeFormat().encoding;
      auto const outputSampleBytes = static_cast<std::size_t>(bytesPerSample(nativeEncoding));
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
                           static_cast<std::uint8_t>(_implPtr->sourceBitsPerSample),
                           encodingNominalBits(nativeEncoding));
      }
    }

    auto convertedRes = _implPtr->outputAdapter.convert(_implPtr->pcmBuffer);

    if (!convertedRes)
    {
      return std::unexpected{convertedRes.error()};
    }

    _implPtr->nextFrameIndex += frames;
    _implPtr->eof = _implPtr->nextFrameIndex == _implPtr->totalFrames;

    return PcmBlock{
      .bytes = *convertedRes, .frames = frames, .firstFrameIndex = currentFrameIndex, .endOfStream = _implPtr->eof};
  }

  DecodedStreamInfo WavDecoderSession::streamInfo() const noexcept
  {
    return _implPtr->info;
  }
} // namespace ao::audio
