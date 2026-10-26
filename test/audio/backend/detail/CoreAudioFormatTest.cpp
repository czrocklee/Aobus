// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/CoreAudioFormat.h"

#include <ao/Error.h>
#include <ao/audio/Backend.h>
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
#include <expected>
#include <functional>
#include <string_view>
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

  TEST_CASE("CoreAudioFormat - maps every lossless encoding to strict interleaved little-endian PCM")
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

  TEST_CASE("CoreAudioFormat - preserves low-aligned 24-bit-in-32 identity")
  {
    auto const nativeRes =
      coreAudioFormat(PcmFormat{.sampleRate = 44100, .channels = 1, .encoding = SampleEncoding::Signed24In32Le});
    REQUIRE(nativeRes);

    CHECK((nativeRes->mFormatFlags & ::kAudioFormatFlagIsPacked) == 0U);
    CHECK((nativeRes->mFormatFlags & ::kAudioFormatFlagIsAlignedHigh) == 0U);
    CHECK(nativeRes->mBitsPerChannel == 24U);
    CHECK(nativeRes->mBytesPerFrame == 4U);
  }

  TEST_CASE("CoreAudioFormat - rejects an unknown source encoding")
  {
    auto const unknownRes =
      coreAudioFormat(PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Unknown});
    REQUIRE_FALSE(unknownRes);
    CHECK(unknownRes.error().code == Error::Code::FormatRejected);
  }

  TEST_CASE("CoreAudioFormat - derives endpoint signal evidence without claiming client layout")
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

  TEST_CASE("CoreAudioFormat - walks only lossless candidates and accepts exact read-back")
  {
    auto attemptedFrameByteCounts = std::vector<::UInt32>{};
    auto const selectedRes = selectLosslessCoreAudioClientFormat(
      {.sampleRate = 48000, .channels = 2, .precisionBits = 24, .sampleKind = SampleKind::Integer},
      [&](::AudioStreamBasicDescription const& candidate)
        -> std::expected<::AudioStreamBasicDescription, BackendOpenFailure>
      {
        attemptedFrameByteCounts.push_back(candidate.mBytesPerFrame);

        if (candidate.mBytesPerFrame == 6U)
        {
          return std::unexpected{BackendOpenFailure{.error = makeError(Error::Code::FormatRejected).error(),
                                                    .cause = BackendOpenFailure::Cause::TrackRouteIncompatible}};
        }

        return candidate;
      });

    REQUIRE(selectedRes);
    CHECK(selectedRes->encoding == SampleEncoding::Signed24In32Le);
    REQUIRE(attemptedFrameByteCounts.size() == 2U);
    CHECK(attemptedFrameByteCounts[0] == 6U);
    CHECK(attemptedFrameByteCounts[1] == 8U);
  }

  TEST_CASE("CoreAudioFormat - selected padded storage retains canonical identity through the candidate walk")
  {
    std::size_t attempts = 0U;
    auto const selectedRes = selectLosslessCoreAudioClientFormat(
      {.sampleRate = 48000, .channels = 2, .precisionBits = 24, .speakerMask = 0x3},
      [&](::AudioStreamBasicDescription const& candidate)
        -> std::expected<::AudioStreamBasicDescription, BackendOpenFailure>
      {
        ++attempts;

        if (attempts == 1U)
        {
          return std::unexpected{
            BackendOpenFailure{.error = makeError(Error::Code::FormatRejected, "packed refused").error(),
                               .cause = BackendOpenFailure::Cause::TrackRouteIncompatible}};
        }

        return candidate;
      });
    REQUIRE(selectedRes);
    CHECK(attempts == 2U);
    CHECK(
      *selectedRes ==
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed24In32Le, .speakerMask = 0x3});
  }

  TEST_CASE("CoreAudioFormat - required readback contradictions are terminal on the first candidate")
  {
    auto mutate = std::function<void(::AudioStreamBasicDescription&)>{};

    SECTION("frame byte contradiction")
    {
      mutate = [](auto& format) { ++format.mBytesPerFrame; };
    }

    SECTION("sample precision contradiction")
    {
      mutate = [](auto& format) { format.mBitsPerChannel = 16U; };
    }

    SECTION("sample rate contradiction")
    {
      mutate = [](auto& format) { format.mSampleRate = 44100.0; };
    }

    SECTION("interleaving contradiction")
    {
      mutate = [](auto& format) { format.mFormatFlags |= ::kAudioFormatFlagIsNonInterleaved; };
    }

    SECTION("channel count contradiction")
    {
      mutate = [](auto& format) { format.mChannelsPerFrame = 1U; };
    }

    std::size_t attempts = 0U;
    auto const selectedRes = selectLosslessCoreAudioClientFormat(
      {.sampleRate = 48000, .channels = 2, .precisionBits = 24},
      [&](::AudioStreamBasicDescription candidate) -> std::expected<::AudioStreamBasicDescription, BackendOpenFailure>
      {
        ++attempts;
        mutate(candidate);
        return candidate;
      });
    REQUIRE_FALSE(selectedRes);
    CHECK(attempts == 1U);
    CHECK(selectedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    CHECK(selectedRes.error().error.code == Error::Code::FormatRejected);
    CHECK(selectedRes.error().error.message == "Core Audio applied a different client PCM mode than requested");
  }

  TEST_CASE("CoreAudioFormat - a mismatched 24-bit read-back stops the walk instead of exhausting candidates")
  {
    std::size_t attempts = 0;
    auto const selectedRes = selectLosslessCoreAudioClientFormat(
      {.sampleRate = 48000, .channels = 2, .precisionBits = 24, .sampleKind = SampleKind::Integer},
      [&](::AudioStreamBasicDescription const& candidate)
        -> std::expected<::AudioStreamBasicDescription, BackendOpenFailure>
      {
        ++attempts;

        // Only the first (packed 24-bit) candidate lies on read-back; a later
        // exact candidate would succeed if the walk wrongly retried it.
        if (attempts > 1U)
        {
          return candidate;
        }

        auto readBack = candidate;
        readBack.mBytesPerPacket = 4U * candidate.mChannelsPerFrame;
        readBack.mBytesPerFrame = 4U * candidate.mChannelsPerFrame;
        readBack.mBitsPerChannel = 32U;
        return readBack;
      });

    REQUIRE_FALSE(selectedRes);
    CHECK(attempts == 1U);
    CHECK(selectedRes.error().error.code == Error::Code::FormatRejected);
    CHECK(selectedRes.error().error.message == "Core Audio applied a different client PCM mode than requested");
    CHECK(selectedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
  }

  TEST_CASE("CoreAudioFormat - rejects coercion and never narrows a 32-bit integer source")
  {
    std::size_t attempts = 0;
    auto const rejectedRes = selectLosslessCoreAudioClientFormat(
      {.sampleRate = 96000, .channels = 2, .precisionBits = 32, .sampleKind = SampleKind::Integer},
      [&](::AudioStreamBasicDescription const&) -> std::expected<::AudioStreamBasicDescription, BackendOpenFailure>
      {
        ++attempts;
        return std::unexpected{BackendOpenFailure{.error = makeError(Error::Code::FormatRejected).error(),
                                                  .cause = BackendOpenFailure::Cause::TrackRouteIncompatible}};
      });

    REQUIRE_FALSE(rejectedRes);
    // An exhausted lossless set keeps the native operation's own diagnostic.
    CHECK(rejectedRes.error().error.code == Error::Code::FormatRejected);
    CHECK(rejectedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    CHECK(attempts == 1U);

    auto const coercedRes = selectLosslessCoreAudioClientFormat(
      {.sampleRate = 48000, .channels = 2, .precisionBits = 32, .sampleKind = SampleKind::FloatingPoint},
      [](::AudioStreamBasicDescription candidate) -> std::expected<::AudioStreamBasicDescription, BackendOpenFailure>
      {
        candidate.mSampleRate = 44100.0;
        return candidate;
      });
    REQUIRE_FALSE(coercedRes);
    // A malformed or mismatched successful read-back is a native output
    // fault, distinct from the track's own format-set exhaustion above.
    CHECK(coercedRes.error().error.code == Error::Code::FormatRejected);
    CHECK(coercedRes.error().error.message == "Core Audio applied a different client PCM mode than requested");
    CHECK(coercedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
  }

  TEST_CASE("CoreAudioFormat - retries only track-incompatible attempt failures")
  {
    std::size_t attempts = 0;
    auto const selectedRes = selectLosslessCoreAudioClientFormat(
      {.sampleRate = 48000, .channels = 2, .precisionBits = 24, .sampleKind = SampleKind::Integer},
      [&](::AudioStreamBasicDescription const&) -> std::expected<::AudioStreamBasicDescription, BackendOpenFailure>
      {
        ++attempts;
        return std::unexpected{BackendOpenFailure{.error = makeError(Error::Code::IoError, "native read fault").error(),
                                                  .cause = BackendOpenFailure::Cause::OutputFailure}};
      });

    // The 24-bit lossless set has several candidates, but a conservative
    // output failure must abort the walk and propagate unchanged.
    REQUIRE_FALSE(selectedRes);
    CHECK(attempts == 1U);
    CHECK(selectedRes.error().error.code == Error::Code::IoError);
    CHECK(selectedRes.error().error.message == "native read fault");
    CHECK(selectedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
  }

  TEST_CASE(
    "CoreAudioFormat - default output failures retain their original diagnostic even with capability-like codes")
  {
    auto code = Error::Code::FormatRejected;

    SECTION("FormatRejected without an explicit incompatibility cause is terminal")
    {
    }

    SECTION("NotSupported without an explicit incompatibility cause is terminal")
    {
      code = Error::Code::NotSupported;
    }

    auto const original = makeError(code, "original native operation context").error();
    std::size_t attempts = 0U;
    auto const selectedRes =
      selectLosslessCoreAudioClientFormat({.sampleRate = 48000, .channels = 2, .precisionBits = 24},
                                          [&](::AudioStreamBasicDescription const& candidate)
                                            -> std::expected<::AudioStreamBasicDescription, BackendOpenFailure>
                                          {
                                            ++attempts;

                                            if (attempts == 1U)
                                            {
                                              return std::unexpected{BackendOpenFailure{.error = original}};
                                            }

                                            return candidate;
                                          });
    REQUIRE_FALSE(selectedRes);
    CHECK(attempts == 1U);
    CHECK(selectedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    CHECK(selectedRes.error().error.code == original.code);
    CHECK(selectedRes.error().error.message == original.message);
    CHECK(std::string_view{selectedRes.error().error.location.file_name()} == original.location.file_name());
    CHECK(std::string_view{selectedRes.error().error.location.function_name()} == original.location.function_name());
    CHECK(selectedRes.error().error.location.line() == original.location.line());
    CHECK(selectedRes.error().error.location.column() == original.location.column());
  }

  TEST_CASE("CoreAudioFormat - an unrepresentable source is refused without a native attempt")
  {
    std::size_t attempts = 0;
    auto const selectedRes = selectLosslessCoreAudioClientFormat(
      {.sampleRate = 48000, .channels = 2, .precisionBits = 33, .sampleKind = SampleKind::Integer},
      [&](::AudioStreamBasicDescription const&) -> std::expected<::AudioStreamBasicDescription, BackendOpenFailure>
      {
        ++attempts;
        return std::unexpected{BackendOpenFailure{.error = makeError(Error::Code::FormatRejected).error(),
                                                  .cause = BackendOpenFailure::Cause::TrackRouteIncompatible}};
      });

    REQUIRE_FALSE(selectedRes);
    CHECK(attempts == 0U);
    CHECK(selectedRes.error().error.code == Error::Code::FormatRejected);
    CHECK(selectedRes.error().error.message == "No lossless Core Audio client PCM encoding is available");
    CHECK(selectedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
  }

  TEST_CASE("CoreAudioFormat - a missing attempt callback is an output failure")
  {
    auto const selectedRes = selectLosslessCoreAudioClientFormat(
      {.sampleRate = 48000, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer}, {});

    REQUIRE_FALSE(selectedRes);
    CHECK(selectedRes.error().error.code == Error::Code::InvalidInput);
    CHECK(selectedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
  }
} // namespace ao::audio::backend::detail::test
