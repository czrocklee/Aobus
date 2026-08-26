// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/CoreAudioFormat.h"

#include <ao/Error.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <CoreAudioTypes/CoreAudioBaseTypes.h>
#include <MacTypes.h>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ao::audio::backend::detail::test
{
  namespace
  {
    struct FormatCase final
    {
      SampleEncoding encoding = SampleEncoding::Unknown;
      ::AudioFormatFlags flags = 0U;
      std::uint32_t sampleBytes = 0U;
      std::uint32_t precisionBits = 0U;
    };

    constexpr auto kFormatCases = std::array{
      FormatCase{SampleEncoding::Signed16Le, ::kAudioFormatFlagIsSignedInteger | ::kAudioFormatFlagIsPacked, 2U, 16U},
      FormatCase{SampleEncoding::Signed24PackedLe,
                 ::kAudioFormatFlagIsSignedInteger | ::kAudioFormatFlagIsPacked,
                 3U,
                 24U},
      FormatCase{SampleEncoding::Signed24In32Le, ::kAudioFormatFlagIsSignedInteger, 4U, 24U},
      FormatCase{SampleEncoding::Signed32Le, ::kAudioFormatFlagIsSignedInteger | ::kAudioFormatFlagIsPacked, 4U, 32U},
      FormatCase{SampleEncoding::Float32Le, ::kAudioFormatFlagIsFloat | ::kAudioFormatFlagIsPacked, 4U, 32U}};
  } // namespace

  TEST_CASE("CoreAudioFormat - maps every lossless encoding to strict interleaved little-endian PCM",
            "[audio][unit][coreaudio]")
  {
    for (auto const& testCase : kFormatCases)
    {
      CAPTURE(testCase.encoding);
      auto const pcm = PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = testCase.encoding};
      auto const nativeRes = coreAudioFormat(pcm);
      REQUIRE(nativeRes);
      auto const& native = *nativeRes;
      CHECK(native.mSampleRate == 48000.0);
      CHECK(native.mFormatID == ::kAudioFormatLinearPCM);
      CHECK(native.mFormatFlags == testCase.flags);
      CHECK(native.mBytesPerPacket == testCase.sampleBytes * 2U);
      CHECK(native.mFramesPerPacket == 1U);
      CHECK(native.mBytesPerFrame == testCase.sampleBytes * 2U);
      CHECK(native.mChannelsPerFrame == 2U);
      CHECK(native.mBitsPerChannel == testCase.precisionBits);
    }
  }

  TEST_CASE("CoreAudioFormat - preserves low-aligned 24-bit-in-32 identity", "[audio][unit][coreaudio]")
  {
    auto const nativeRes =
      coreAudioFormat(PcmFormat{.sampleRate = 44100, .channels = 1, .encoding = SampleEncoding::Signed24In32Le});
    REQUIRE(nativeRes);

    CHECK((nativeRes->mFormatFlags & ::kAudioFormatFlagIsPacked) == 0U);
    CHECK((nativeRes->mFormatFlags & ::kAudioFormatFlagIsAlignedHigh) == 0U);
    CHECK(nativeRes->mBitsPerChannel == 24U);
    CHECK(nativeRes->mBytesPerFrame == 4U);
  }

  TEST_CASE("CoreAudioFormat - rejects an unknown source encoding", "[audio][unit][coreaudio]")
  {
    auto const unknownRes =
      coreAudioFormat(PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Unknown});
    REQUIRE_FALSE(unknownRes);
    CHECK(unknownRes.error().code == Error::Code::FormatRejected);
  }

  TEST_CASE("CoreAudioFormat - derives endpoint signal evidence without claiming client layout",
            "[audio][unit][coreaudio]")
  {
    auto native =
      *coreAudioFormat(PcmFormat{.sampleRate = 96000, .channels = 2, .encoding = SampleEncoding::Float32Le});
    native.mFormatFlags |= ::kAudioFormatFlagIsNonInterleaved;

    auto const signalRes = coreAudioSignalFormat(native);
    REQUIRE(signalRes);
    CHECK(signalRes->sampleRate == 96000U);
    CHECK(signalRes->channels == 2U);
    CHECK(signalRes->precisionBits == 32U);
    CHECK(signalRes->sampleKind == SampleKind::FloatingPoint);
  }

  TEST_CASE("CoreAudioFormat - walks only lossless candidates and accepts exact read-back", "[audio][unit][coreaudio]")
  {
    auto attemptedFrameByteCounts = std::vector<::UInt32>{};
    auto const selectedRes = selectLosslessCoreAudioClientFormat(
      {.sampleRate = 48000, .channels = 2, .precisionBits = 24, .sampleKind = SampleKind::Integer},
      [&](::AudioStreamBasicDescription const& candidate) -> Result<::AudioStreamBasicDescription>
      {
        attemptedFrameByteCounts.push_back(candidate.mBytesPerFrame);

        if (candidate.mBytesPerFrame == 6U)
        {
          return makeError(Error::Code::FormatRejected);
        }

        return candidate;
      });

    REQUIRE(selectedRes);
    CHECK(selectedRes->encoding == SampleEncoding::Signed24In32Le);
    REQUIRE(attemptedFrameByteCounts.size() == 2U);
    CHECK(attemptedFrameByteCounts[0] == 6U);
    CHECK(attemptedFrameByteCounts[1] == 8U);
  }

  TEST_CASE("CoreAudioFormat - rejects coercion and never narrows a 32-bit integer source", "[audio][unit][coreaudio]")
  {
    std::size_t attempts = 0;
    auto const rejectedRes = selectLosslessCoreAudioClientFormat(
      {.sampleRate = 96000, .channels = 2, .precisionBits = 32, .sampleKind = SampleKind::Integer},
      [&](::AudioStreamBasicDescription const&) -> Result<::AudioStreamBasicDescription>
      {
        ++attempts;
        return makeError(Error::Code::FormatRejected);
      });

    REQUIRE_FALSE(rejectedRes);
    CHECK(rejectedRes.error().code == Error::Code::FormatRejected);
    CHECK(attempts == 1U);

    auto const coercedRes = selectLosslessCoreAudioClientFormat(
      {.sampleRate = 48000, .channels = 2, .precisionBits = 32, .sampleKind = SampleKind::FloatingPoint},
      [](::AudioStreamBasicDescription candidate) -> Result<::AudioStreamBasicDescription>
      {
        candidate.mSampleRate = 44100.0;
        return candidate;
      });
    REQUIRE_FALSE(coercedRes);
    CHECK(coercedRes.error().code == Error::Code::FormatRejected);
  }
} // namespace ao::audio::backend::detail::test
