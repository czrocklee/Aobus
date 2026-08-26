// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CoreAudioFormat.h"

#include "detail/DecoderOutput.h"
#include <ao/Error.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <CoreAudioTypes/CoreAudioBaseTypes.h>
#include <MacTypes.h>

#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>

namespace ao::audio::backend::detail
{
  namespace
  {
    Result<::AudioFormatFlags> formatFlags(SampleEncoding const encoding)
    {
      switch (encoding)
      {
        case SampleEncoding::Signed16Le:
        case SampleEncoding::Signed24PackedLe:
        case SampleEncoding::Signed32Le: return ::kAudioFormatFlagIsSignedInteger | ::kAudioFormatFlagIsPacked;
        case SampleEncoding::Signed24In32Le: return ::kAudioFormatFlagIsSignedInteger;
        case SampleEncoding::Float32Le: return ::kAudioFormatFlagIsFloat | ::kAudioFormatFlagIsPacked;
        case SampleEncoding::Unknown:
          return makeError(Error::Code::FormatRejected, "Core Audio cannot represent an unknown PCM encoding");
      }

      return makeError(Error::Code::FormatRejected, "Core Audio cannot represent this PCM encoding");
    }
  } // namespace

  Result<::AudioStreamBasicDescription> coreAudioFormat(PcmFormat const& format)
  {
    if (format.sampleRate == 0U || format.channels == 0U)
    {
      return makeError(Error::Code::InvalidInput, "Core Audio PCM formats require a sample rate and channels");
    }

    auto const flagsRes = formatFlags(format.encoding);

    if (!flagsRes)
    {
      return std::unexpected{flagsRes.error()};
    }

    auto const sampleBytes = bytesPerSample(format.encoding);
    auto const frameByteCount = static_cast<std::uint64_t>(format.channels) * sampleBytes;

    if (frameByteCount > std::numeric_limits<::UInt32>::max())
    {
      return makeError(Error::Code::ValueTooLarge, "Core Audio PCM frame size is too large");
    }

    return ::AudioStreamBasicDescription{.mSampleRate = static_cast<::Float64>(format.sampleRate),
                                         .mFormatID = ::kAudioFormatLinearPCM,
                                         .mFormatFlags = *flagsRes,
                                         .mBytesPerPacket = static_cast<::UInt32>(frameByteCount),
                                         .mFramesPerPacket = 1U,
                                         .mBytesPerFrame = static_cast<::UInt32>(frameByteCount),
                                         .mChannelsPerFrame = format.channels,
                                         .mBitsPerChannel = encodingNominalBits(format.encoding),
                                         .mReserved = 0U};
  }

  Result<SignalFormat> coreAudioSignalFormat(::AudioStreamBasicDescription const& format)
  {
    auto const floatingPoint = (format.mFormatFlags & ::kAudioFormatFlagIsFloat) != 0U;
    auto const signedInteger = (format.mFormatFlags & ::kAudioFormatFlagIsSignedInteger) != 0U;

    if (format.mFormatID != ::kAudioFormatLinearPCM || floatingPoint == signedInteger || format.mSampleRate <= 0.0 ||
        !std::isfinite(format.mSampleRate) || std::trunc(format.mSampleRate) != format.mSampleRate ||
        format.mSampleRate > static_cast<::Float64>(std::numeric_limits<std::uint32_t>::max()) ||
        format.mChannelsPerFrame == 0U || format.mChannelsPerFrame > std::numeric_limits<std::uint8_t>::max() ||
        format.mBitsPerChannel == 0U || format.mBitsPerChannel > std::numeric_limits<std::uint8_t>::max())
    {
      return makeError(Error::Code::FormatRejected, "Core Audio returned an invalid endpoint signal format");
    }

    return SignalFormat{.sampleRate = static_cast<std::uint32_t>(format.mSampleRate),
                        .channels = static_cast<std::uint8_t>(format.mChannelsPerFrame),
                        .precisionBits = static_cast<std::uint8_t>(format.mBitsPerChannel),
                        .sampleKind = floatingPoint ? SampleKind::FloatingPoint : SampleKind::Integer};
  }

  Result<PcmFormat> selectLosslessCoreAudioClientFormat(SignalFormat const& sourceFormat,
                                                        TryCoreAudioClientFormat const& tryFormat)
  {
    if (!tryFormat)
    {
      return makeError(Error::Code::InvalidInput, "Core Audio format selection requires an attempt callback");
    }

    for (auto const encoding : ::ao::audio::detail::losslessPcmEncodings(sourceFormat))
    {
      auto const candidate = pcmFormat(sourceFormat, encoding);
      auto const descriptionRes = coreAudioFormat(candidate);

      if (!descriptionRes)
      {
        return std::unexpected{descriptionRes.error()};
      }

      auto const readBackRes = tryFormat(*descriptionRes);

      if (!readBackRes)
      {
        if (readBackRes.error().code == Error::Code::FormatRejected)
        {
          continue;
        }

        return std::unexpected{readBackRes.error()};
      }

      if (sameCoreAudioPcmFormat(*descriptionRes, *readBackRes))
      {
        return candidate;
      }
    }

    return makeError(Error::Code::FormatRejected, "Core Audio rejected every lossless client PCM format");
  }

  bool sameCoreAudioPcmFormat(::AudioStreamBasicDescription const& lhs,
                              ::AudioStreamBasicDescription const& rhs) noexcept
  {
    return lhs.mSampleRate == rhs.mSampleRate && lhs.mFormatID == rhs.mFormatID &&
           lhs.mFormatFlags == rhs.mFormatFlags && lhs.mBytesPerPacket == rhs.mBytesPerPacket &&
           lhs.mFramesPerPacket == rhs.mFramesPerPacket && lhs.mBytesPerFrame == rhs.mBytesPerFrame &&
           lhs.mChannelsPerFrame == rhs.mChannelsPerFrame && lhs.mBitsPerChannel == rhs.mBitsPerChannel;
  }
} // namespace ao::audio::backend::detail
