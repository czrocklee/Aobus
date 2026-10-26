// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "DecoderTestSupport.h"
#include "lib/audio/FlacDecoderSession.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/FlacFrameTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/SampleEncoding.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    // Independently authored FLAC byte streams: a STREAMINFO block declaring
    // 48000 Hz, 2 channels, 16 bits, followed by fixed-blocksize frames of 16
    // verbatim samples each. Frame markers are hand-picked so that a format
    // regression is visible in the decoded samples (for example 257 and -513
    // truncate to 1 and -3 if a 24-bit frame is decoded as 16-bit).
    constexpr std::uint32_t kStreamSampleRate = 48000;
    constexpr std::uint8_t kStreamChannels = 2;
    constexpr std::uint8_t kStreamBitsPerSample = 16;
    constexpr std::uint32_t kFrameSamples = 16;
    constexpr std::uint64_t kTotalSamples = 32;

    constexpr auto kFirstFrameMarkers = std::to_array<std::int32_t>({100, -200});
    constexpr auto kSecondFrameMarkers = std::to_array<std::int32_t>({257, -513, 1027});

    std::vector<std::uint8_t> buildTwoFrameStream(std::uint32_t secondFrameRate,
                                                  std::uint8_t secondFrameChannels,
                                                  std::uint8_t secondFrameBits)
    {
      auto bytes = buildFlacStreaminfoBytes(kTotalSamples);
      auto const firstFrame =
        buildFlacVerbatimFrameBytes(0, kStreamSampleRate, kStreamChannels, kStreamBitsPerSample, kFirstFrameMarkers);
      bytes.insert(bytes.end(), firstFrame.begin(), firstFrame.end());
      auto const secondFrame =
        buildFlacVerbatimFrameBytes(1, secondFrameRate, secondFrameChannels, secondFrameBits, kSecondFrameMarkers);
      bytes.insert(bytes.end(), secondFrame.begin(), secondFrame.end());

      return bytes;
    }

    std::vector<std::uint8_t> buildStableTwoFrameStream()
    {
      return buildTwoFrameStream(kStreamSampleRate, kStreamChannels, kStreamBitsPerSample);
    }

    std::vector<std::uint8_t> buildFirstFrameMismatchStream()
    {
      auto bytes = buildFlacStreaminfoBytes(kTotalSamples);
      auto const changedFirstFrame =
        buildFlacVerbatimFrameBytes(0, kStreamSampleRate, kStreamChannels, 24, kSecondFrameMarkers);
      bytes.insert(bytes.end(), changedFirstFrame.begin(), changedFirstFrame.end());
      auto const secondFrame =
        buildFlacVerbatimFrameBytes(1, kStreamSampleRate, kStreamChannels, kStreamBitsPerSample, kSecondFrameMarkers);
      bytes.insert(bytes.end(), secondFrame.begin(), secondFrame.end());

      return bytes;
    }

    void checkStreamInfoAgainstStream(DecodedStreamInfo const& info)
    {
      CHECK(info.sourceFormat.sampleRate == kStreamSampleRate);
      CHECK(info.sourceFormat.channels == kStreamChannels);
      CHECK(info.sourceFormat.precisionBits == kStreamBitsPerSample);
      CHECK(info.outputFormat.sampleRate == kStreamSampleRate);
      CHECK(info.outputFormat.channels == kStreamChannels);
      CHECK(info.codec == AudioCodec::Flac);
      CHECK_FALSE(info.isLossy);
    }

    void checkFirstFrameBlock(PcmBlock const& block)
    {
      CHECK(block.frames == kFrameSamples);
      CHECK(block.firstFrameIndex == 0);
      CHECK_FALSE(block.endOfStream);
      REQUIRE(block.bytes.size() == 64);

      for (std::size_t frame = 0; frame < block.frames; ++frame)
      {
        CHECK(readSigned16LePcmSample(block.bytes, frame * 2U) == kFirstFrameMarkers[0]);
        CHECK(readSigned16LePcmSample(block.bytes, (frame * 2U) + 1U) == kFirstFrameMarkers[1]);
      }
    }

    void checkPreservedError(Error const& actual, Error const& original)
    {
      // A preserved failure is the recorded cause itself rather than a fixed
      // test phrase: compare it relative to the original, including its
      // borrowed diagnostic location strings.
      CHECK(actual.code == original.code);
      CHECK(actual.message == original.message);
      CHECK(actual.location.line() == original.location.line());
      CHECK(std::string_view{actual.location.file_name()} == std::string_view{original.location.file_name()});
      CHECK(std::string_view{actual.location.function_name()} == std::string_view{original.location.function_name()});
    }

    void checkContradictingFrameRejected(std::vector<std::uint8_t> const& stream, std::string_view expectedContext)
    {
      auto const temp = ao::test::TempFile{stream, ".flac"};
      auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
      auto& decoder = *decoderPtr;

      auto const info = decoder.streamInfo();
      checkStreamInfoAgainstStream(info);
      CHECK(info.outputFormat.encoding == SampleEncoding::Signed16Le);

      auto const firstBlockRes = decoder.readNextBlock();
      REQUIRE(firstBlockRes);
      checkFirstFrameBlock(*firstBlockRes);

      auto const offendingRes = decoder.readNextBlock();
      REQUIRE_FALSE(offendingRes);
      CHECK(offendingRes.error().code == Error::Code::DecodeFailed);
      CHECK_FALSE(offendingRes.error().message.empty());
      CHECK(offendingRes.error().message.contains(expectedContext));

      // The failure is sticky: later reads report the same recorded error
      // instead of converting it into a fake end of stream or partial PCM.
      for (std::uint32_t repeat = 0; repeat < 2; ++repeat)
      {
        auto const repeatedRes = decoder.readNextBlock();
        REQUIRE_FALSE(repeatedRes);
        checkPreservedError(repeatedRes.error(), offendingRes.error());
      }

      CHECK(decoder.streamInfo() == info);
    }
  } // namespace

  TEST_CASE("FlacDecoderSession - stable two-frame stream decodes complete authored PCM")
  {
    auto const temp = ao::test::TempFile{buildStableTwoFrameStream(), ".flac"};
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
    auto& decoder = *decoderPtr;

    auto const info = decoder.streamInfo();
    checkStreamInfoAgainstStream(info);
    CHECK(info.outputFormat.encoding == SampleEncoding::Signed16Le);
    CHECK(info.duration == std::chrono::milliseconds{0});

    auto const firstBlockRes = decoder.readNextBlock();
    REQUIRE(firstBlockRes);
    checkFirstFrameBlock(*firstBlockRes);

    auto const secondBlockRes = decoder.readNextBlock();
    REQUIRE(secondBlockRes);
    CHECK(secondBlockRes->frames == kFrameSamples);
    CHECK(secondBlockRes->firstFrameIndex == kFrameSamples);
    CHECK_FALSE(secondBlockRes->endOfStream);
    REQUIRE(secondBlockRes->bytes.size() == 64);

    for (std::size_t frame = 0; frame < secondBlockRes->frames; ++frame)
    {
      CHECK(readSigned16LePcmSample(secondBlockRes->bytes, frame * 2U) == kSecondFrameMarkers[0]);
      CHECK(readSigned16LePcmSample(secondBlockRes->bytes, (frame * 2U) + 1U) == kSecondFrameMarkers[1]);
    }

    auto const endRes = decoder.readNextBlock();
    REQUIRE(endRes);
    CHECK(endRes->endOfStream);
    CHECK(endRes->frames == 0);
    CHECK(endRes->bytes.empty());
  }

  TEST_CASE("FlacDecoderSession - requested output encoding adapts the stable stream")
  {
    auto const temp = ao::test::TempFile{buildStableTwoFrameStream(), ".flac"};
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, SampleEncoding::Signed32Le));
    auto& decoder = *decoderPtr;

    auto const info = decoder.streamInfo();
    CHECK(info.outputFormat.encoding == SampleEncoding::Signed32Le);

    auto const firstBlockRes = decoder.readNextBlock();
    REQUIRE(firstBlockRes);
    CHECK(firstBlockRes->bytes.size() == 128);

    for (std::size_t frame = 0; frame < firstBlockRes->frames; ++frame)
    {
      CHECK(readSigned32LePcmSample(firstBlockRes->bytes, frame * 2U) == kFirstFrameMarkers[0] * 65536);
      CHECK(readSigned32LePcmSample(firstBlockRes->bytes, (frame * 2U) + 1U) == kFirstFrameMarkers[1] * 65536);
    }

    auto const secondBlockRes = decoder.readNextBlock();
    REQUIRE(secondBlockRes);
    CHECK(secondBlockRes->bytes.size() == 128);

    for (std::size_t frame = 0; frame < secondBlockRes->frames; ++frame)
    {
      CHECK(readSigned32LePcmSample(secondBlockRes->bytes, frame * 2U) == kSecondFrameMarkers[0] * 65536);
      CHECK(readSigned32LePcmSample(secondBlockRes->bytes, (frame * 2U) + 1U) == kSecondFrameMarkers[1] * 65536);
    }
  }

  TEST_CASE("FlacDecoderSession - requested output encoding still rejects a contradicting frame")
  {
    auto const temp = ao::test::TempFile{buildTwoFrameStream(kStreamSampleRate, 3, kStreamBitsPerSample), ".flac"};
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, SampleEncoding::Signed32Le));
    auto& decoder = *decoderPtr;

    auto const info = decoder.streamInfo();
    CHECK(info.outputFormat.encoding == SampleEncoding::Signed32Le);

    auto const firstBlockRes = decoder.readNextBlock();
    REQUIRE(firstBlockRes);
    CHECK(firstBlockRes->bytes.size() == 128);
    CHECK(readSigned32LePcmSample(firstBlockRes->bytes, 0) == kFirstFrameMarkers[0] * 65536);

    auto const offendingRes = decoder.readNextBlock();
    REQUIRE_FALSE(offendingRes);
    CHECK(offendingRes.error().code == Error::Code::DecodeFailed);
    CHECK_FALSE(offendingRes.error().message.empty());

    CHECK(decoder.streamInfo() == info);
  }

  TEST_CASE("FlacDecoderSession - later frames contradicting the stream format are rejected")
  {
    SECTION("Channel count differs from STREAMINFO")
    {
      checkContradictingFrameRejected(buildTwoFrameStream(kStreamSampleRate, 3, kStreamBitsPerSample), "3 channels");
    }

    SECTION("Bit depth differs from STREAMINFO")
    {
      checkContradictingFrameRejected(
        buildTwoFrameStream(kStreamSampleRate, kStreamChannels, 24), "24 bits per sample");
    }

    SECTION("Sample rate differs from STREAMINFO")
    {
      checkContradictingFrameRejected(buildTwoFrameStream(44100, kStreamChannels, kStreamBitsPerSample), "44100");
    }
  }

  TEST_CASE("FlacDecoderSession - first frame contradicting the stream fails before any PCM")
  {
    auto const temp = ao::test::TempFile{buildFirstFrameMismatchStream(), ".flac"};
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
    auto& decoder = *decoderPtr;

    auto const info = decoder.streamInfo();
    checkStreamInfoAgainstStream(info);
    CHECK(info.outputFormat.encoding == SampleEncoding::Signed16Le);

    auto const firstReadRes = decoder.readNextBlock();
    REQUIRE_FALSE(firstReadRes);
    CHECK(firstReadRes.error().code == Error::Code::DecodeFailed);
    CHECK_FALSE(firstReadRes.error().message.empty());

    auto const repeatedRes = decoder.readNextBlock();
    REQUIRE_FALSE(repeatedRes);
    checkPreservedError(repeatedRes.error(), firstReadRes.error());

    CHECK(decoder.streamInfo() == info);
  }

  TEST_CASE("FlacDecoderSession - seeking into a contradicting frame preserves the failure")
  {
    auto const temp = ao::test::TempFile{buildTwoFrameStream(kStreamSampleRate, 3, kStreamBitsPerSample), ".flac"};
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
    auto& decoder = *decoderPtr;

    auto const info = decoder.streamInfo();

    auto const firstBlockRes = decoder.readNextBlock();
    REQUIRE(firstBlockRes);
    checkFirstFrameBlock(*firstBlockRes);

    // Seeking back into the intact first frame still succeeds.
    REQUIRE(decoder.seek(std::chrono::milliseconds{0}));
    auto const soughtBlockRes = decoder.readNextBlock();
    REQUIRE(soughtBlockRes);
    checkFirstFrameBlock(*soughtBlockRes);

    // 1 ms at 48 kHz clamps to the last declared sample, which the changed
    // second frame owns; the seek fails with the preserved format error.
    auto const seekRes = decoder.seek(std::chrono::milliseconds{1});
    REQUIRE_FALSE(seekRes);
    CHECK(seekRes.error().code == Error::Code::DecodeFailed);
    CHECK_FALSE(seekRes.error().message.empty());

    auto const readAfterFailedSeekRes = decoder.readNextBlock();
    REQUIRE_FALSE(readAfterFailedSeekRes);
    checkPreservedError(readAfterFailedSeekRes.error(), seekRes.error());

    CHECK(decoder.streamInfo() == info);
  }

  TEST_CASE("FlacDecoderSession - flush and seek restore decoding before a contradicting frame")
  {
    auto const temp = ao::test::TempFile{buildTwoFrameStream(kStreamSampleRate, kStreamChannels, 24), ".flac"};
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
    auto& decoder = *decoderPtr;

    auto const info = decoder.streamInfo();

    auto const firstBlockRes = decoder.readNextBlock();
    REQUIRE(firstBlockRes);
    checkFirstFrameBlock(*firstBlockRes);

    auto const offendingRes = decoder.readNextBlock();
    REQUIRE_FALSE(offendingRes);
    CHECK(offendingRes.error().code == Error::Code::DecodeFailed);
    CHECK_FALSE(offendingRes.error().message.empty());

    // Flush resets the codec buffers but does not mask the stored failure.
    decoder.flush();
    auto const afterFlushRes = decoder.readNextBlock();
    REQUIRE_FALSE(afterFlushRes);
    checkPreservedError(afterFlushRes.error(), offendingRes.error());

    // A later seek clears the stored failure and restarts decoding at the
    // intact first frame; the contradicting frame fails again on arrival.
    REQUIRE(decoder.seek(std::chrono::milliseconds{0}));
    auto const recoveredBlockRes = decoder.readNextBlock();
    REQUIRE(recoveredBlockRes);
    checkFirstFrameBlock(*recoveredBlockRes);

    auto const repeatRes = decoder.readNextBlock();
    REQUIRE_FALSE(repeatRes);
    CHECK(repeatRes.error().code == Error::Code::DecodeFailed);

    CHECK(decoder.streamInfo() == info);
  }

  TEST_CASE("FlacDecoderSession - seek without flush cannot clear a terminal decode failure")
  {
    // Both entry paths into the terminal failure state share this arrange: the
    // intact first frame decodes, and the changed second frame owns the last
    // declared sample of the stream.
    auto const temp = ao::test::TempFile{buildTwoFrameStream(kStreamSampleRate, 3, kStreamBitsPerSample), ".flac"};
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
    auto& decoder = *decoderPtr;

    auto const info = decoder.streamInfo();

    auto const firstBlockRes = decoder.readNextBlock();
    REQUIRE(firstBlockRes);
    checkFirstFrameBlock(*firstBlockRes);

    SECTION("Failure entered by a rejected read")
    {
      auto const offendingRes = decoder.readNextBlock();
      REQUIRE_FALSE(offendingRes);
      CHECK(offendingRes.error().code == Error::Code::DecodeFailed);
      CHECK_FALSE(offendingRes.error().message.empty());

      // A seek without flush cannot clear the recorded failure: it is refused
      // with the preserved cause, including its diagnostic location.
      auto const seekRes = decoder.seek(std::chrono::milliseconds{0});
      REQUIRE_FALSE(seekRes);
      checkPreservedError(seekRes.error(), offendingRes.error());

      CHECK(decoder.streamInfo() == info);

      // Reads keep reporting the recorded failure instead of looping or
      // converting it into a fake end of stream or partial PCM.
      for (std::uint32_t repeat = 0; repeat < 2; ++repeat)
      {
        auto const repeatedRes = decoder.readNextBlock();
        REQUIRE_FALSE(repeatedRes);
        checkPreservedError(repeatedRes.error(), offendingRes.error());
      }

      // The explicit recovery protocol still restores decoding, and the
      // changed frame fails again on arrival.
      decoder.flush();
      REQUIRE(decoder.seek(std::chrono::milliseconds{0}));
      auto const recoveredBlockRes = decoder.readNextBlock();
      REQUIRE(recoveredBlockRes);
      checkFirstFrameBlock(*recoveredBlockRes);

      auto const repeatRes = decoder.readNextBlock();
      REQUIRE_FALSE(repeatRes);
      CHECK(repeatRes.error().code == Error::Code::DecodeFailed);

      CHECK(decoder.streamInfo() == info);
    }

    SECTION("Failure entered by a seek-time format abort")
    {
      // Re-enter the intact first frame before seeking into the changed
      // second frame, exercising a seek-target format failure.
      REQUIRE(decoder.seek(std::chrono::milliseconds{0}));
      auto const rereadRes = decoder.readNextBlock();
      REQUIRE(rereadRes);

      // 1 ms at 48 kHz clamps to the last declared sample, which the changed
      // second frame owns; the seek aborts with the format failure.
      auto const abortSeekRes = decoder.seek(std::chrono::milliseconds{1});
      REQUIRE_FALSE(abortSeekRes);
      CHECK(abortSeekRes.error().code == Error::Code::DecodeFailed);
      CHECK_FALSE(abortSeekRes.error().message.empty());

      // Another seek without flush preserves the recorded failure together
      // with its diagnostic location instead of clearing it.
      auto const seekRes = decoder.seek(std::chrono::milliseconds{0});
      REQUIRE_FALSE(seekRes);
      checkPreservedError(seekRes.error(), abortSeekRes.error());

      CHECK(decoder.streamInfo() == info);

      // A read after the failed seek keeps reporting the recorded failure
      // instead of looping or producing a fake end of stream.
      auto const readRes = decoder.readNextBlock();
      REQUIRE_FALSE(readRes);
      checkPreservedError(readRes.error(), abortSeekRes.error());

      // The explicit recovery protocol still restores decoding, and the
      // changed frame fails again on arrival.
      decoder.flush();
      REQUIRE(decoder.seek(std::chrono::milliseconds{0}));
      auto const recoveredBlockRes = decoder.readNextBlock();
      REQUIRE(recoveredBlockRes);
      checkFirstFrameBlock(*recoveredBlockRes);

      auto const repeatRes = decoder.readNextBlock();
      REQUIRE_FALSE(repeatRes);
      CHECK(repeatRes.error().code == Error::Code::DecodeFailed);

      CHECK(decoder.streamInfo() == info);
    }
  }

  TEST_CASE("FlacDecoderSession - seek failure without a stored format cause repeats without flush")
  {
    // The STREAMINFO-only stream declares samples but contains no frames, so
    // seeking has no frame to land on and records no format cause: the
    // failure must stay a generic seek failure instead of turning into a
    // success or a stuck decoder.
    auto const temp = ao::test::TempFile{buildFlacStreaminfoBytes(kTotalSamples), ".flac"};
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
    auto& decoder = *decoderPtr;

    auto const info = decoder.streamInfo();

    auto const seekRes = decoder.seek(std::chrono::milliseconds{0});
    REQUIRE_FALSE(seekRes);
    CHECK(seekRes.error().code == Error::Code::SeekFailed);
    CHECK_FALSE(seekRes.error().message.empty());

    // Another seek without flush cannot turn the failure into a success.
    auto const repeatSeekRes = decoder.seek(std::chrono::milliseconds{0});
    REQUIRE_FALSE(repeatSeekRes);
    CHECK(repeatSeekRes.error().code == Error::Code::SeekFailed);
    CHECK_FALSE(repeatSeekRes.error().message.empty());

    CHECK(decoder.streamInfo() == info);

    // Reads keep failing instead of looping or producing a fake end of stream.
    for (std::uint32_t repeat = 0; repeat < 2; ++repeat)
    {
      auto const readRes = decoder.readNextBlock();
      REQUIRE_FALSE(readRes);
      CHECK(readRes.error().code == Error::Code::DecodeFailed);
      CHECK_FALSE(readRes.error().message.empty());
    }

    CHECK(decoder.streamInfo() == info);
  }

  TEST_CASE("FlacDecoderSession - flush and seek do not recover a stream with no frames")
  {
    // The STREAMINFO-only stream declares 32 samples but contains no frames,
    // so a seek records no frame to land on and no decoding cause. Neither
    // the explicit flush+seek recovery protocol nor a repeated seek conjures
    // the missing frames back.
    auto const temp = ao::test::TempFile{buildFlacStreaminfoBytes(kTotalSamples), ".flac"};
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
    auto& decoder = *decoderPtr;

    auto const info = decoder.streamInfo();

    auto const initialSeekRes = decoder.seek(std::chrono::milliseconds{0});
    REQUIRE_FALSE(initialSeekRes);
    CHECK(initialSeekRes.error().code == Error::Code::SeekFailed);
    CHECK_FALSE(initialSeekRes.error().message.empty());

    for (std::uint32_t round = 0; round < 2; ++round)
    {
      // Flushing resets the codec, but the declared samples still have no
      // frames to land on: the recovery protocol cannot restart the stream.
      decoder.flush();

      auto const flushedSeekRes = decoder.seek(std::chrono::milliseconds{0});
      REQUIRE_FALSE(flushedSeekRes);
      CHECK(flushedSeekRes.error().code == Error::Code::SeekFailed);
      CHECK_FALSE(flushedSeekRes.error().message.empty());

      // A repeated seek without flush cannot turn the failure into a success.
      auto const repeatedSeekRes = decoder.seek(std::chrono::milliseconds{0});
      REQUIRE_FALSE(repeatedSeekRes);
      CHECK(repeatedSeekRes.error().code == Error::Code::SeekFailed);
      CHECK_FALSE(repeatedSeekRes.error().message.empty());
    }

    CHECK(decoder.streamInfo() == info);

    // Reads from the failed codec keep returning DecodeFailed rather than
    // looping, producing PCM, or converting the failure into a fake EOF.
    for (std::uint32_t repeat = 0; repeat < 3; ++repeat)
    {
      auto const readRes = decoder.readNextBlock();
      REQUIRE_FALSE(readRes);
      CHECK(readRes.error().code == Error::Code::DecodeFailed);
      CHECK_FALSE(readRes.error().message.empty());
    }

    CHECK(decoder.streamInfo() == info);
  }
} // namespace ao::audio::test
