// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/NodeFormat.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace ao::audio::test
{
  namespace
  {
    constexpr PcmFormat makeFormat(std::uint32_t sampleRate, std::uint8_t channels, SampleEncoding encoding)
    {
      return PcmFormat{.sampleRate = sampleRate, .channels = channels, .encoding = encoding};
    }
  } // namespace

  TEST_CASE("SampleEncoding - container width is explicit", "[audio][unit][format]")
  {
    CHECK(bytesPerSample(SampleEncoding::Signed16Le) == 2U);
    CHECK(bytesPerSample(SampleEncoding::Signed24PackedLe) == 3U);
    CHECK(bytesPerSample(SampleEncoding::Signed24In32Le) == 4U);
    CHECK(bytesPerSample(SampleEncoding::Signed32Le) == 4U);
    CHECK(bytesPerSample(SampleEncoding::Float32Le) == 4U);
  }

  TEST_CASE("PcmFormat - frameBytes accounts for channel count", "[audio][unit][format]")
  {
    CHECK(frameBytes(makeFormat(44100, 2, SampleEncoding::Signed16Le)) == 4U);
    CHECK(frameBytes(makeFormat(44100, 1, SampleEncoding::Signed16Le)) == 2U);
    CHECK(frameBytes(makeFormat(44100, 2, SampleEncoding::Signed24PackedLe)) == 6U);
    CHECK(frameBytes(makeFormat(48000, 6, SampleEncoding::Signed32Le)) == 24U);
  }

  TEST_CASE("PcmFormat - frameBytes/bytesPerSecond return 0 for unconfigured formats", "[audio][unit][format]")
  {
    CHECK(frameBytes(makeFormat(44100, 0, SampleEncoding::Signed16Le)) == 0U);
    CHECK(frameBytes(PcmFormat{.sampleRate = 44100, .channels = 2}) == 0U);

    CHECK(bytesPerSecond(makeFormat(0, 2, SampleEncoding::Signed16Le)) == 0U);
    CHECK(bytesPerSecond(makeFormat(44100, 0, SampleEncoding::Signed16Le)) == 0U);
    CHECK(bytesPerSecond(PcmFormat{.sampleRate = 44100, .channels = 2}) == 0U);
  }

  TEST_CASE("PcmFormat - bytesPerSecond is sampleRate * frameBytes", "[audio][unit][format]")
  {
    CHECK(bytesPerSecond(makeFormat(44100, 2, SampleEncoding::Signed16Le)) == static_cast<std::uint64_t>(44100) * 4U);
    CHECK(bytesPerSecond(makeFormat(48000, 2, SampleEncoding::Signed24PackedLe)) ==
          static_cast<std::uint64_t>(48000) * 6U);
    CHECK(bytesPerSecond(makeFormat(96000, 2, SampleEncoding::Signed32Le)) == static_cast<std::uint64_t>(96000) * 8U);
  }

  TEST_CASE("Format helpers - signalFormat and pcmFormat copy the speaker mask", "[audio][unit][format]")
  {
    auto const identifiedPcm =
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x3};

    auto const signal = signalFormat(identifiedPcm);

    CHECK(signal.sampleRate == 48000U);
    CHECK(signal.channels == 2U);
    CHECK(signal.precisionBits == 16U);
    CHECK(signal.sampleKind == SampleKind::Integer);
    CHECK(signal.speakerMask == 0x3U);

    auto const roundTripped = pcmFormat(signal, SampleEncoding::Signed16Le);
    CHECK(roundTripped == identifiedPcm);
  }

  TEST_CASE("NodeFormat - signalFormat roundtrips the speaker mask through either alternative", "[audio][unit][format]")
  {
    auto const identifiedPcm =
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x3};
    auto const pcmNode = NodeFormat{identifiedPcm};

    auto const signal = signalFormat(pcmNode);

    CHECK(signal.speakerMask == 0x3U);
    CHECK(pcmFormat(signal, SampleEncoding::Signed16Le) == identifiedPcm);

    auto const signalNode =
      NodeFormat{SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 0x3}};
    CHECK(signalFormat(signalNode) == signal);
  }

  TEST_CASE("PcmFormat - same-count modes with different speaker masks are different PCM modes",
            "[audio][unit][format]")
  {
    auto const unassigned = makeFormat(48000, 2, SampleEncoding::Signed16Le);
    auto const frontStereo =
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x3};
    auto const frontStereoCopy =
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x3};

    CHECK(frontStereo == frontStereoCopy);
    CHECK(unassigned != frontStereo);
    CHECK_FALSE(isSamePcmMode(unassigned, frontStereo));
    CHECK(isSamePcmMode(frontStereo, frontStereoCopy));
  }

  TEST_CASE("SignalFormat - complete WAVE masks assign one supported role to every channel", "[audio][unit][format]")
  {
    CHECK(isCompleteWaveSpeakerMask(0x4, 1));
    CHECK(isCompleteWaveSpeakerMask(0x30, 2));
    CHECK(isCompleteWaveSpeakerMask(0x3FFFF, 18));
    CHECK_FALSE(isCompleteWaveSpeakerMask(0, 0));
    CHECK_FALSE(isCompleteWaveSpeakerMask(0, 2));
    CHECK_FALSE(isCompleteWaveSpeakerMask(0x1, 2));
    CHECK_FALSE(isCompleteWaveSpeakerMask(0x7, 2));
    CHECK_FALSE(isCompleteWaveSpeakerMask(0x40000, 1));
    CHECK_FALSE(isCompleteWaveSpeakerMask(0x80000000, 1));
  }

  TEST_CASE("SignalFormat - same-count signals with different speaker masks are not equal", "[audio][unit][format]")
  {
    auto const unassigned = SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16};
    auto const frontStereo = SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 0x3};

    CHECK(unassigned != frontStereo);
    CHECK(frontStereo == SignalFormat{frontStereo});
  }

  TEST_CASE("PcmFormat - frameBytes follows channel count and encoding, not speaker population",
            "[audio][unit][format]")
  {
    auto const twoChannel = [&](std::uint32_t const speakerMask)
    {
      return PcmFormat{
        .sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = speakerMask};
    };

    // Unassigned, matching, partial, and extra speaker populations all describe
    // two S16 channels; the channel count stays authoritative for byte geometry.
    CHECK(frameBytes(twoChannel(0U)) == 4U);
    CHECK(frameBytes(twoChannel(0x3)) == 4U);
    CHECK(frameBytes(twoChannel(0x1)) == 4U);
    CHECK(frameBytes(twoChannel(0x7)) == 4U);

    CHECK(bytesPerSecond(twoChannel(0x3)) == static_cast<std::uint64_t>(48000) * 4U);
  }

  TEST_CASE("DecodedStreamInfo - channel-layout evidence alone distinguishes otherwise equal streams",
            "[audio][unit][format]")
  {
    auto const sourceFormat = SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 0x3};
    auto const outputFormat =
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x3};

    auto const unexamined = DecodedStreamInfo{.sourceFormat = sourceFormat, .outputFormat = outputFormat};
    auto const identified = DecodedStreamInfo{.sourceFormat = sourceFormat,
                                              .outputFormat = outputFormat,
                                              .sourceChannelLayout = {ChannelLayoutOrigin::Identified}};

    CHECK(unexamined.sourceFormat == identified.sourceFormat);
    CHECK(unexamined.outputFormat == identified.outputFormat);
    CHECK(unexamined != identified);
    CHECK(unexamined == DecodedStreamInfo{unexamined});
  }

  // The helpers must be usable in constant expressions (RT-path callers depend on
  // them folding away).
  static_assert(frameBytes(PcmFormat{.sampleRate = 44100, .channels = 2, .encoding = SampleEncoding::Signed16Le}) ==
                4U);
  static_assert(bytesPerSecond(PcmFormat{.sampleRate = 1000, .channels = 1, .encoding = SampleEncoding::Signed16Le}) ==
                2000U);

  // A speaker mask is copied identity, never stride; a nonzero mask never turns
  // a two-channel S16 mode into a different byte geometry.
  static_assert(signalFormat(PcmFormat{.sampleRate = 48000,
                                       .channels = 2,
                                       .encoding = SampleEncoding::Signed16Le,
                                       .speakerMask = 0x3})
                  .speakerMask == 0x3U);
  static_assert(pcmFormat(SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 0x3},
                          SampleEncoding::Signed16Le)
                  .speakerMask == 0x3U);
  static_assert(frameBytes(PcmFormat{.sampleRate = 48000,
                                     .channels = 2,
                                     .encoding = SampleEncoding::Signed16Le,
                                     .speakerMask = 0x7}) == 4U);
  static_assert(!isSamePcmMode(
    PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le},
    PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x3}));
} // namespace ao::audio::test
