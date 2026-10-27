// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "DecoderTestSupport.h"
#include "lib/audio/FlacDecoderSession.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/audio/FlacFrameTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/SampleEncoding.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    // Authored fixture geometry shared with FlacFrameTestSupport: 48000 Hz,
    // 2 channels, 16 bits, and 16-sample frames.
    constexpr std::uint32_t kStreamSampleRate = 48000;
    constexpr std::uint8_t kStreamChannels = 2;
    constexpr std::uint8_t kStreamBitsPerSample = 16;
    constexpr std::uint32_t kFrameSamples = 16;
    constexpr std::uint64_t kDeclaredTotalSamples = 48;

    // Hand-picked constant markers, distinct per frame and channel, so a
    // wrong frame or wrong sample position is visible in the decoded PCM.
    constexpr auto kFirstFrameMarkers = std::to_array<std::int32_t>({100, -200});
    constexpr auto kMiddleFrameMarkers = std::to_array<std::int32_t>({200, -300});
    constexpr auto kLastFrameMarkers = std::to_array<std::int32_t>({300, -400});

    void appendBytes(std::vector<std::uint8_t>& bytes, std::vector<std::uint8_t> const& suffix)
    {
      bytes.insert(bytes.end(), suffix.begin(), suffix.end());
    }

    std::vector<std::uint8_t> buildFrameBytes(std::uint32_t frameIndex, std::span<std::int32_t const> markers)
    {
      return buildFlacVerbatimFrameBytes(frameIndex, kStreamSampleRate, kStreamChannels, kStreamBitsPerSample, markers);
    }

    std::vector<std::uint8_t> corruptFrameSyncByte(std::vector<std::uint8_t> frameBytes)
    {
      REQUIRE(!frameBytes.empty());

      // Alter only the leading frame-sync byte; the changed frame is not
      // claimed to be header- or CRC-valid.
      frameBytes[0] = 0xFE;
      return frameBytes;
    }

    std::vector<std::uint8_t> buildStableFrameBodyBytes()
    {
      auto bytes = buildFrameBytes(0, kFirstFrameMarkers);
      appendBytes(bytes, buildFrameBytes(1, kMiddleFrameMarkers));
      appendBytes(bytes, buildFrameBytes(2, kLastFrameMarkers));

      return bytes;
    }

    std::vector<std::uint8_t> buildStableStreamBytes(std::uint64_t totalSamples)
    {
      auto bytes = buildFlacStreaminfoBytes(totalSamples);
      appendBytes(bytes, buildStableFrameBodyBytes());

      return bytes;
    }

    std::vector<std::uint8_t> buildCorruptedMiddleSyncStreamBytes(std::uint64_t totalSamples)
    {
      auto bytes = buildFlacStreaminfoBytes(totalSamples);
      appendBytes(bytes, buildFrameBytes(0, kFirstFrameMarkers));
      appendBytes(bytes, corruptFrameSyncByte(buildFrameBytes(1, kMiddleFrameMarkers)));
      appendBytes(bytes, buildFrameBytes(2, kLastFrameMarkers));

      return bytes;
    }

    std::vector<std::uint8_t> buildCorruptedFirstSyncStreamBytes()
    {
      auto bytes = buildFlacStreaminfoBytes(0);
      appendBytes(bytes, corruptFrameSyncByte(buildFrameBytes(0, kFirstFrameMarkers)));
      appendBytes(bytes, buildFrameBytes(1, kMiddleFrameMarkers));
      appendBytes(bytes, buildFrameBytes(2, kLastFrameMarkers));

      return bytes;
    }

    std::vector<std::uint8_t> buildFirstFrameOmittedStreamBytes()
    {
      // The declared total is unknown, so no end-of-stream sample-count check
      // can catch the missing first frame either.
      auto bytes = buildFlacStreaminfoBytes(0);
      appendBytes(bytes, buildFrameBytes(1, kMiddleFrameMarkers));
      appendBytes(bytes, buildFrameBytes(2, kLastFrameMarkers));

      return bytes;
    }

    void checkFrameBlock(PcmBlock const& block, std::uint64_t firstFrameIndex, std::span<std::int32_t const> markers)
    {
      CHECK(block.frames == kFrameSamples);
      CHECK(block.firstFrameIndex == firstFrameIndex);
      CHECK_FALSE(block.endOfStream);
      REQUIRE(block.bytes.size() == 64);

      for (std::size_t frame = 0; frame < block.frames; ++frame)
      {
        CHECK(readSigned16LePcmSample(block.bytes, frame * 2U) == markers[0]);
        CHECK(readSigned16LePcmSample(block.bytes, (frame * 2U) + 1U) == markers[1]);
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

    void checkCorruptedMiddleSyncRejected(std::uint64_t totalSamples)
    {
      auto const temp = ao::test::TempFile{buildCorruptedMiddleSyncStreamBytes(totalSamples), ".flac"};
      auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
      auto& decoder = *decoderPtr;

      auto const info = decoder.streamInfo();

      auto const firstBlockRes = decoder.readNextBlock();
      REQUIRE(firstBlockRes);
      checkFrameBlock(*firstBlockRes, 0, kFirstFrameMarkers);

      // The provider repairs the lost middle frame with silence and then
      // delivers the last frame during the same single operation; this second
      // public read must fail before that misplaced PCM becomes visible.
      auto const offendingRes = decoder.readNextBlock();
      REQUIRE_FALSE(offendingRes);
      CHECK(offendingRes.error().code == Error::Code::DecodeFailed);
      CHECK_FALSE(offendingRes.error().message.empty());

      // The failure is sticky: later reads report the same recorded cause
      // instead of converting it into a fake end of stream or partial PCM.
      for (std::uint32_t repeat = 0; repeat < 2; ++repeat)
      {
        auto const repeatedRes = decoder.readNextBlock();
        REQUIRE_FALSE(repeatedRes);
        checkPreservedError(repeatedRes.error(), offendingRes.error());
      }

      // A seek without flush preserves the recorded failure together with
      // its diagnostic location.
      auto const seekRes = decoder.seek(std::chrono::milliseconds{0});
      REQUIRE_FALSE(seekRes);
      checkPreservedError(seekRes.error(), offendingRes.error());

      // A flush alone resets the codec buffers but does not clear the cause.
      decoder.flush();
      auto const afterFlushRes = decoder.readNextBlock();
      REQUIRE_FALSE(afterFlushRes);
      checkPreservedError(afterFlushRes.error(), offendingRes.error());

      // The explicit recovery protocol restarts at the intact first frame,
      // and the corrupted middle frame fails again on arrival.
      REQUIRE(decoder.seek(std::chrono::milliseconds{0}));
      auto const recoveredBlockRes = decoder.readNextBlock();
      REQUIRE(recoveredBlockRes);
      checkFrameBlock(*recoveredBlockRes, 0, kFirstFrameMarkers);

      auto const repeatRes = decoder.readNextBlock();
      REQUIRE_FALSE(repeatRes);
      CHECK(repeatRes.error().code == Error::Code::DecodeFailed);

      CHECK(decoder.streamInfo() == info);
    }

    void checkFirstReadRejected(std::vector<std::uint8_t> const& stream)
    {
      auto const temp = ao::test::TempFile{stream, ".flac"};
      auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
      auto& decoder = *decoderPtr;

      auto const info = decoder.streamInfo();

      auto const firstReadRes = decoder.readNextBlock();
      REQUIRE_FALSE(firstReadRes);
      CHECK(firstReadRes.error().code == Error::Code::DecodeFailed);
      CHECK_FALSE(firstReadRes.error().message.empty());

      auto const repeatedRes = decoder.readNextBlock();
      REQUIRE_FALSE(repeatedRes);
      checkPreservedError(repeatedRes.error(), firstReadRes.error());

      CHECK(decoder.streamInfo() == info);
    }

    void checkOpenRejected(std::vector<std::uint8_t> const& stream)
    {
      auto const temp = ao::test::TempFile{stream, ".flac"};
      auto const res = FlacDecoderSession::open(temp.path, std::nullopt);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::DecodeFailed);
      CHECK_FALSE(res.error().message.empty());
    }
  } // namespace

  TEST_CASE("FlacDecoderSession - malformed stream handling", "[audio][unit][flac][malformed]")
  {
    auto const fixture = requireAudioFixture("basic_metadata.flac");
    auto const source = readFileBytes(fixture);
    CHECK(source.size() > 1024);

    SECTION("Truncated STREAMINFO fails during open")
    {
      auto data = source;
      data.resize(16);
      auto const temp = ao::test::TempFile{data, ".flac"};
      auto const res = FlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::DecodeFailed);
    }

    SECTION("Truncated audio reaches a stable terminal state")
    {
      auto data = source;
      data.resize(data.size() / 2);
      auto const temp = ao::test::TempFile{data, ".flac"};
      auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le));
      auto& decoder = *decoderPtr;
      auto const terminal = readUntilTerminalState(decoder, 512);

      CHECK(terminal.frames > 0);
      CHECK(terminal.frames < 44100);
      REQUIRE(terminal.optError);
      CHECK(terminal.optError->code == Error::Code::DecodeFailed);

      auto const repeatedReadRes = decoder.readNextBlock();
      REQUIRE_FALSE(repeatedReadRes);
      CHECK(repeatedReadRes.error().code == Error::Code::DecodeFailed);
    }

    SECTION("Corrupt audio frame does not hang or overrun the stream")
    {
      auto data = source;
      auto const corruptionBegin = data.size() / 2;
      auto const corruptionEnd = corruptionBegin + 256;
      REQUIRE(corruptionEnd < data.size());
      std::fill(data.begin() + static_cast<std::ptrdiff_t>(corruptionBegin),
                data.begin() + static_cast<std::ptrdiff_t>(corruptionEnd),
                0xA5);

      auto const temp = ao::test::TempFile{data, ".flac"};
      auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le));
      auto& decoder = *decoderPtr;
      auto const terminal = readUntilTerminalState(decoder, 512);

      CHECK(terminal.frames > 0);
      CHECK(terminal.frames < 44100);
      REQUIRE(terminal.optError);
      CHECK(terminal.optError->code == Error::Code::DecodeFailed);

      auto const repeatedReadRes = decoder.readNextBlock();
      REQUIRE_FALSE(repeatedReadRes);
      CHECK(repeatedReadRes.error().code == Error::Code::DecodeFailed);
    }
  }

  TEST_CASE("FlacDecoderSession - corrupted middle frame sync is rejected before misplaced PCM",
            "[audio][unit][flac][malformed]")
  {
    // Three stable 16-sample frames with one changed sync byte in the middle
    // frame: the provider pads the gap with silence and delivers the last
    // frame early, so the second read must fail before any wrong PCM. Both a
    // declared and an unknown total share the contract.
    SECTION("Declared total of 48 samples")
    {
      checkCorruptedMiddleSyncRejected(kDeclaredTotalSamples);
    }

    SECTION("Unknown total")
    {
      checkCorruptedMiddleSyncRejected(0);
    }
  }

  TEST_CASE("FlacDecoderSession - lost first frame with unknown total fails the first read",
            "[audio][unit][flac][malformed]")
  {
    // With an unknown declared total, no end-of-stream sample-count check can
    // catch a lost first frame, and the provider performs no silent padding
    // write here: the first read itself must reject the discontinuous
    // delivery instead of relabeling later PCM as sample 0.
    SECTION("First frame sync corrupted")
    {
      checkFirstReadRejected(buildCorruptedFirstSyncStreamBytes());
    }

    SECTION("First frame cleanly omitted")
    {
      checkFirstReadRejected(buildFirstFrameOmittedStreamBytes());
    }
  }

  TEST_CASE("FlacDecoderSession - cleanly omitted middle frame is rejected before later PCM",
            "[audio][unit][flac][malformed]")
  {
    // Removing the middle frame without any preceding sync loss makes the
    // provider report a missing frame; the read must fail before the intact
    // last frame is published with wrong sample indices.
    auto bytes = buildFlacStreaminfoBytes(kDeclaredTotalSamples);
    appendBytes(bytes, buildFrameBytes(0, kFirstFrameMarkers));
    appendBytes(bytes, buildFrameBytes(2, kLastFrameMarkers));

    auto const temp = ao::test::TempFile{bytes, ".flac"};
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
    auto& decoder = *decoderPtr;

    auto const info = decoder.streamInfo();

    auto const firstBlockRes = decoder.readNextBlock();
    REQUIRE(firstBlockRes);
    checkFrameBlock(*firstBlockRes, 0, kFirstFrameMarkers);

    auto const offendingRes = decoder.readNextBlock();
    REQUIRE_FALSE(offendingRes);
    CHECK(offendingRes.error().code == Error::Code::DecodeFailed);
    CHECK_FALSE(offendingRes.error().message.empty());

    auto const repeatedRes = decoder.readNextBlock();
    REQUIRE_FALSE(repeatedRes);
    checkPreservedError(repeatedRes.error(), offendingRes.error());

    CHECK(decoder.streamInfo() == info);
  }

  TEST_CASE("FlacDecoderSession - stray bytes between frames resynchronize without losing samples",
            "[audio][unit][flac][malformed]")
  {
    // Positive control for gap rejection: harmless noise between otherwise
    // contiguous frames only makes the provider resynchronize. Every sample
    // must still be delivered at its literal index with the full marker PCM,
    // the stream must reach its normal end, and the info must stay unchanged.
    auto bytes = buildFlacStreaminfoBytes(kDeclaredTotalSamples);
    appendBytes(bytes, buildFrameBytes(0, kFirstFrameMarkers));
    // 'x', 'y', 'z': bytes that cannot form a frame sync.
    bytes.push_back(0x78);
    bytes.push_back(0x79);
    bytes.push_back(0x7A);
    appendBytes(bytes, buildFrameBytes(1, kMiddleFrameMarkers));
    appendBytes(bytes, buildFrameBytes(2, kLastFrameMarkers));

    auto const temp = ao::test::TempFile{bytes, ".flac"};
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
    auto& decoder = *decoderPtr;

    auto const info = decoder.streamInfo();

    auto const firstBlockRes = decoder.readNextBlock();
    REQUIRE(firstBlockRes);
    checkFrameBlock(*firstBlockRes, 0, kFirstFrameMarkers);

    auto const middleBlockRes = decoder.readNextBlock();
    REQUIRE(middleBlockRes);
    checkFrameBlock(*middleBlockRes, kFrameSamples, kMiddleFrameMarkers);

    auto const lastBlockRes = decoder.readNextBlock();
    REQUIRE(lastBlockRes);
    checkFrameBlock(*lastBlockRes, 32, kLastFrameMarkers);

    for (std::uint32_t repeat = 0; repeat < 2; ++repeat)
    {
      auto const endRes = decoder.readNextBlock();
      REQUIRE(endRes);
      CHECK(endRes->endOfStream);
      CHECK(endRes->frames == 0);
      CHECK(endRes->bytes.empty());
    }

    CHECK(decoder.streamInfo() == info);
  }

  TEST_CASE("FlacDecoderSession - clamped seek target returns only the remaining samples", "[audio][unit][flac]")
  {
    // 1 ms at 48 kHz is one sample past the 48 declared samples, so the seek
    // clamps to the last sample and the provider trims the final frame to a
    // single sample at the requested index. Because no terminal failure was
    // recorded, a later seek can replay the stream without flushing.
    auto const temp = ao::test::TempFile{buildStableStreamBytes(kDeclaredTotalSamples), ".flac"};
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
    auto& decoder = *decoderPtr;

    auto const info = decoder.streamInfo();

    REQUIRE(decoder.seek(std::chrono::milliseconds{1}));

    auto const tailRes = decoder.readNextBlock();
    REQUIRE(tailRes);
    CHECK(tailRes->frames == 1);
    CHECK(tailRes->firstFrameIndex == kDeclaredTotalSamples - 1U);
    REQUIRE(tailRes->bytes.size() == 4);
    CHECK(readSigned16LePcmSample(tailRes->bytes, 0) == kLastFrameMarkers[0]);
    CHECK(readSigned16LePcmSample(tailRes->bytes, 1) == kLastFrameMarkers[1]);

    auto const endRes = decoder.readNextBlock();
    REQUIRE(endRes);
    CHECK(endRes->endOfStream);
    CHECK(endRes->frames == 0);
    CHECK(endRes->bytes.empty());

    // A healthy seek without flush replays the original first frame in full.
    REQUIRE(decoder.seek(std::chrono::milliseconds{0}));
    auto const replayRes = decoder.readNextBlock();
    REQUIRE(replayRes);
    checkFrameBlock(*replayRes, 0, kFirstFrameMarkers);

    // The seek-target override ends with the synchronous seek: subsequent
    // sequential reads must advance beyond that target rather than reuse it.
    auto const middleRes = decoder.readNextBlock();
    REQUIRE(middleRes);
    checkFrameBlock(*middleRes, kFrameSamples, kMiddleFrameMarkers);
    auto const lastRes = decoder.readNextBlock();
    REQUIRE(lastRes);
    checkFrameBlock(*lastRes, 32, kLastFrameMarkers);
    auto const replayEndRes = decoder.readNextBlock();
    REQUIRE(replayEndRes);
    CHECK(replayEndRes->endOfStream);
    CHECK(replayEndRes->frames == 0);
    CHECK(replayEndRes->bytes.empty());

    CHECK(decoder.streamInfo() == info);
  }

  TEST_CASE("FlacDecoderSession - healthy flush preserves unread PCM and decode position", "[audio][unit][flac]")
  {
    // This tiny file fits entirely in provider read-ahead. Flushing must not
    // skip those unread bytes or discard a decoded but unread seek target.
    auto const temp = ao::test::TempFile{buildStableStreamBytes(kDeclaredTotalSamples), ".flac"};
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
    auto& decoder = *decoderPtr;
    auto const info = decoder.streamInfo();

    SECTION("Sequential read-ahead resumes at the next frame")
    {
      auto const firstRes = decoder.readNextBlock();
      REQUIRE(firstRes);
      checkFrameBlock(*firstRes, 0, kFirstFrameMarkers);
      decoder.flush();
      auto const middleRes = decoder.readNextBlock();
      REQUIRE(middleRes);
      checkFrameBlock(*middleRes, kFrameSamples, kMiddleFrameMarkers);
      auto const lastRes = decoder.readNextBlock();
      REQUIRE(lastRes);
      checkFrameBlock(*lastRes, 32, kLastFrameMarkers);
    }

    SECTION("Already decoded seek-target PCM remains consumable")
    {
      REQUIRE(decoder.seek(std::chrono::milliseconds{1}));
      decoder.flush();
      auto const tailRes = decoder.readNextBlock();
      REQUIRE(tailRes);
      CHECK(tailRes->frames == 1);
      CHECK(tailRes->firstFrameIndex == 47);
      REQUIRE(tailRes->bytes.size() == 4);
      CHECK(readSigned16LePcmSample(tailRes->bytes, 0) == kLastFrameMarkers[0]);
      CHECK(readSigned16LePcmSample(tailRes->bytes, 1) == kLastFrameMarkers[1]);
    }

    auto const endRes = decoder.readNextBlock();
    REQUIRE(endRes);
    CHECK(endRes->endOfStream);
    CHECK(endRes->frames == 0);
    CHECK(endRes->bytes.empty());
    CHECK(decoder.streamInfo() == info);
  }

  TEST_CASE("FlacDecoderSession - open rejects a stream without usable STREAMINFO", "[audio][unit][flac][malformed]")
  {
    SECTION("fLaC with only a last zero-length PADDING block")
    {
      auto const bytes = std::vector<std::uint8_t>{'f', 'L', 'a', 'C', 0x81, 0x00, 0x00, 0x00};
      checkOpenRejected(bytes);
    }

    SECTION("Zero-length PADDING followed by valid frames")
    {
      auto bytes = std::vector<std::uint8_t>{'f', 'L', 'a', 'C', 0x81, 0x00, 0x00, 0x00};
      appendBytes(bytes, buildStableFrameBodyBytes());
      checkOpenRejected(bytes);
    }

    SECTION("STREAMINFO declaring a zero sample rate")
    {
      // The STREAMINFO packed field lives in bytes 18-25: the 20-bit sample
      // rate occupies the top bits, so zeroing bytes 18-19 and the high
      // nibble of byte 20 clears the rate without touching the channel,
      // bit-depth, or total-sample bits.
      auto bytes = buildFlacStreaminfoBytes(kDeclaredTotalSamples);
      bytes[18] = 0x00;
      bytes[19] = 0x00;
      bytes[20] = static_cast<std::uint8_t>(bytes[20] & 0x0F);
      appendBytes(bytes, buildStableFrameBodyBytes());
      checkOpenRejected(bytes);
    }
  }
} // namespace ao::audio::test
