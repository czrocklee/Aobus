// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/SegmentDecoderSession.h>

#include "FlacFrameTestSupport.h"
#include "ScriptedDecoderSession.h"
#include "lib/audio/DecoderFactory.h"
#include "lib/audio/detail/TrackSession.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/media/wav/TestWav.h"
#include <ao/Error.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    constexpr std::uint64_t kCarrierFrames = 2048;
    constexpr std::uint32_t kSampleRate = 44100;

    std::vector<std::uint8_t> carrierPcm()
    {
      auto bytes = std::vector<std::uint8_t>{};

      for (std::uint32_t frame = 0; frame < kCarrierFrames; ++frame)
      {
        auto const marker = static_cast<std::uint16_t>((frame / 16U) + 1U);
        ao::test::wav::appendLe16(bytes, marker);
        ao::test::wav::appendLe16(bytes, static_cast<std::uint16_t>(0U - marker));
      }

      return bytes;
    }

    std::vector<std::uint8_t> carrierBytes(bool flac)
    {
      if (!flac)
      {
        return ao::test::wav::makeWav({.channels = 2, .sampleRate = kSampleRate, .audioData = carrierPcm()});
      }

      auto bytes = buildFlacStreaminfoBytes(kCarrierFrames);
      auto const packed =
        (static_cast<std::uint64_t>(kSampleRate) << 44U) | (1ULL << 41U) | (15ULL << 36U) | kCarrierFrames;

      for (std::size_t index = 0; index < 8; ++index)
      {
        bytes[18 + index] = static_cast<std::uint8_t>(packed >> ((7 - index) * 8U));
      }

      for (std::uint32_t index = 0; index < 128; ++index)
      {
        auto const marker = static_cast<std::int32_t>(index + 1);
        auto const markers = std::to_array<std::int32_t>({marker, -marker});
        auto const frame = buildFlacVerbatimFrameBytes(index, kSampleRate, 2, 16, markers);
        bytes.insert(bytes.end(), frame.begin(), frame.end());
      }

      return bytes;
    }

    std::vector<std::byte> readPcm(DecoderSession& decoder, std::uint64_t firstFrame = 0)
    {
      auto bytes = std::vector<std::byte>{};
      auto nextFrame = firstFrame;

      for (std::size_t index = 0; index < 256; ++index)
      {
        auto const block = ao::test::requireValue(decoder.readNextBlock());

        if (block.frames != 0)
        {
          CHECK(block.firstFrameIndex == nextFrame);
          CHECK(block.bytes.size() == block.frames * frameBytes(decoder.streamInfo().outputFormat));
          bytes.insert(bytes.end(), block.bytes.begin(), block.bytes.end());
          nextFrame += block.frames;
        }

        if (block.endOfStream)
        {
          auto const end = ao::test::requireValue(decoder.readNextBlock());
          CHECK(end.frames == 0);
          CHECK(end.bytes.empty());
          CHECK(end.endOfStream);
          return bytes;
        }
      }

      FAIL("Segment decoder did not reach EOF");
      return bytes;
    }

    std::unique_ptr<DecoderSession> openSegment(std::filesystem::path const& path,
                                                PlaybackSegment const& segment,
                                                std::uint32_t expectedRate = kSampleRate)
    {
      return ao::test::requireValue(makeSegmentDecoderSession(
        ao::test::requireValue(openDecoderSession(path, SampleEncoding::Signed16Le)), segment, expectedRate));
    }

    // A frame-seek capable source that lands before its target. The declared
    // extent and physical payload differ only for the premature-EOF case.
    class [[nodiscard]] FrameSeekDecoderFake final : public DecoderSession
    {
    public:
      explicit FrameSeekDecoderFake(std::uint32_t availableFrames)
        : _availableFrames{availableFrames}
        , _bytes{std::vector<std::byte>(static_cast<std::size_t>(availableFrames) * 2U, std::byte{0x35})}
      {
      }

      Result<> seek(std::chrono::milliseconds /*offset*/) noexcept override
      {
        return makeError(Error::Code::NotSupported);
      }
      bool supportsFrameSeek() const noexcept override { return true; }
      Result<> seekFrame(std::uint64_t frameIndex) noexcept override
      {
        if (_failsSeeks)
        {
          return makeError(Error::Code::SeekFailed, "Scripted frame seek failure");
        }

        _nextFrame = frameIndex / 16U * 16U;
        return {};
      }
      // A failed seek leaves the read position where it was.
      void failSeeks(bool const fails) noexcept { _failsSeeks = fails; }
      void flush() noexcept override {}
      Result<PcmBlock> readNextBlock() noexcept override
      {
        if (_nextFrame >= _availableFrames)
        {
          return PcmBlock{.endOfStream = true};
        }

        auto const firstFrame = _nextFrame;
        auto const frames = static_cast<std::uint32_t>(std::min<std::uint64_t>(16, _availableFrames - _nextFrame));
        _nextFrame += frames;
        return PcmBlock{
          .bytes = std::span<std::byte const>{_bytes}.subspan(firstFrame * 2U, static_cast<std::size_t>(frames) * 2U),
          .frames = frames,
          .firstFrameIndex = firstFrame,
          .endOfStream = _nextFrame == _availableFrames};
      }
      DecodedStreamInfo streamInfo() const noexcept override
      {
        return {.sourceFormat = {.sampleRate = 1000, .channels = 1, .precisionBits = 16},
                .outputFormat = {.sampleRate = 1000, .channels = 1, .encoding = SampleEncoding::Signed16Le},
                .optTotalFrames = 64};
      }

    private:
      std::uint32_t _availableFrames;
      std::vector<std::byte> _bytes;
      std::uint64_t _nextFrame = 0;
      bool _failsSeeks = false;
    };
  } // namespace

  TEST_CASE("SegmentDecoderSession - FLAC and WAV PCM equals the exact carrier region",
            "[audio][integration][segment-decoder]")
  {
    auto const flac = GENERATE(false, true);
    auto const frameCount = GENERATE(std::uint64_t{588}, std::uint64_t{574});
    auto const temp = ao::test::TempFile{carrierBytes(flac), flac ? ".flac" : ".wav"};
    auto const segment = PlaybackSegment{.startFrame = 317, .frameCount = frameCount, .totalFrames = kCarrierFrames};
    auto decoderPtr = openSegment(temp.path, segment);
    CHECK(decoderPtr->streamInfo().duration == std::chrono::milliseconds{13});
    CHECK(decoderPtr->streamInfo().optTotalFrames == frameCount);
    auto const expected = carrierPcm();
    auto const region = utility::bytes::view(std::span<std::uint8_t const>{expected})
                          .subspan(static_cast<std::size_t>(317) * 4, frameCount * 4);
    CHECK(std::ranges::equal(readPcm(*decoderPtr), region));

    REQUIRE(decoderPtr->seek(std::chrono::milliseconds{13}));
    auto const tail = readPcm(*decoderPtr, 573);
    CHECK(tail.size() == (frameCount - 573) * 4);
    CHECK(std::ranges::equal(tail, region.subspan(static_cast<std::size_t>(573) * 4)));

    REQUIRE(decoderPtr->seekFrame(frameCount - 1));
    CHECK(std::ranges::equal(readPcm(*decoderPtr, frameCount - 1), region.last(4)));
    REQUIRE(decoderPtr->seekFrame(frameCount));
    CHECK(readPcm(*decoderPtr).empty());
    REQUIRE(decoderPtr->seek(std::chrono::milliseconds::max()));
    CHECK(readPcm(*decoderPtr).empty());
    REQUIRE(decoderPtr->seek(std::chrono::milliseconds{0}));
    CHECK(std::ranges::equal(readPcm(*decoderPtr), region));
    auto const negativeRes = decoderPtr->seek(std::chrono::milliseconds{-1});
    REQUIRE_FALSE(negativeRes);
    CHECK(negativeRes.error().code == Error::Code::SeekFailed);
  }

  TEST_CASE("SegmentDecoderSession - converted interleaved spans preserve the exact region",
            "[audio][integration][segment-decoder]")
  {
    auto const flac = GENERATE(false, true);
    auto const encoding =
      GENERATE(SampleEncoding::Signed24PackedLe, SampleEncoding::Signed32Le, SampleEncoding::Float32Le);
    auto const temp = ao::test::TempFile{carrierBytes(flac), flac ? ".flac" : ".wav"};
    auto referencePtr = ao::test::requireValue(openDecoderSession(temp.path, encoding));
    auto const reference = readPcm(*referencePtr);
    auto decoderPtr = ao::test::requireValue(
      makeSegmentDecoderSession(ao::test::requireValue(openDecoderSession(temp.path, encoding)),
                                {.startFrame = 317, .frameCount = 588, .totalFrames = kCarrierFrames},
                                kSampleRate));
    auto const bytesPerFrame = frameBytes(decoderPtr->streamInfo().outputFormat);
    CHECK(std::ranges::equal(
      readPcm(*decoderPtr), std::span<std::byte const>{reference}.subspan(317 * bytesPerFrame, 588 * bytesPerFrame)));
  }

  TEST_CASE("SegmentDecoderSession - submillisecond nonempty segments still play at zero",
            "[audio][integration][segment-decoder]")
  {
    auto const flac = GENERATE(false, true);
    auto const temp = ao::test::TempFile{carrierBytes(flac), flac ? ".flac" : ".wav"};
    auto decoderPtr = openSegment(temp.path, {.startFrame = 43, .frameCount = 1, .totalFrames = kCarrierFrames});
    CHECK(decoderPtr->streamInfo().duration == std::chrono::milliseconds{0});
    REQUIRE(decoderPtr->seek(std::chrono::milliseconds{0}));
    auto const expected = carrierPcm();
    CHECK(std::ranges::equal(
      readPcm(*decoderPtr),
      utility::bytes::view(std::span<std::uint8_t const>{expected}).subspan(static_cast<std::size_t>(43) * 4, 4)));
  }

  TEST_CASE("SegmentDecoderSession - rejects changed carrier facts and invalid ranges before PCM",
            "[audio][integration][segment-decoder]")
  {
    auto const flac = GENERATE(false, true);
    auto const temp = ao::test::TempFile{carrierBytes(flac), flac ? ".flac" : ".wav"};
    auto segment = PlaybackSegment{.startFrame = 317, .frameCount = 588, .totalFrames = kCarrierFrames};
    std::uint32_t expectedRate = kSampleRate;

    SECTION("changed rate")
    {
      expectedRate = 48000;
    }

    SECTION("absent stored rate")
    {
      expectedRate = 0;
    }

    SECTION("changed extent")
    {
      ++segment.totalFrames;
    }

    SECTION("empty range")
    {
      segment.frameCount = 0;
    }

    SECTION("range exceeds extent")
    {
      segment.frameCount = kCarrierFrames;
    }

    SECTION("range arithmetic would overflow")
    {
      segment.startFrame = std::numeric_limits<std::uint64_t>::max();
      segment.frameCount = 2;
    }

    auto res = makeSegmentDecoderSession(
      ao::test::requireValue(openDecoderSession(temp.path, std::nullopt)), segment, expectedRate);
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::FormatRejected);
  }

  TEST_CASE("SegmentDecoderSession - codecs without frame seek or exact extent are refused",
            "[audio][unit][segment-decoder]")
  {
    auto info = DecodedStreamInfo{};
    info.optTotalFrames = 64;
    auto res = makeSegmentDecoderSession(
      std::make_unique<ScriptedDecoderSession>(info), {.frameCount = 32, .totalFrames = 64}, 1000);
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::NotSupported);
  }

  TEST_CASE("SegmentDecoderSession - a FLAC without an exact STREAMINFO extent is refused",
            "[audio][integration][segment-decoder]")
  {
    auto bytes = carrierBytes(true);
    bytes[21] &= 0xF0U;
    std::fill(bytes.begin() + 22, bytes.begin() + 26, std::uint8_t{0});
    auto const temp = ao::test::TempFile{bytes, ".flac"};
    auto sourcePtr = ao::test::requireValue(openDecoderSession(temp.path, std::nullopt));
    CHECK_FALSE(sourcePtr->streamInfo().optTotalFrames);
    auto const res =
      makeSegmentDecoderSession(std::move(sourcePtr), {.frameCount = 588, .totalFrames = kCarrierFrames}, kSampleRate);
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::NotSupported);
  }

  TEST_CASE("SegmentDecoderSession - discards leading frames using declared block coordinates",
            "[audio][unit][segment-decoder]")
  {
    auto decoderPtr = ao::test::requireValue(makeSegmentDecoderSession(
      std::make_unique<FrameSeekDecoderFake>(64), {.startFrame = 19, .frameCount = 20, .totalFrames = 64}, 1000));
    CHECK(readPcm(*decoderPtr).size() == 40);
    REQUIRE(decoderPtr->seekFrame(19));
    CHECK(readPcm(*decoderPtr, 19).size() == 2);
  }

  TEST_CASE("SegmentDecoderSession - an inner frame seek failure reaches the caller", "[audio][unit][segment-decoder]")
  {
    constexpr auto kSegment = PlaybackSegment{.startFrame = 19, .frameCount = 20, .totalFrames = 64};
    auto refusedPtr = std::make_unique<FrameSeekDecoderFake>(64);
    refusedPtr->failSeeks(true);
    // Opening positions the carrier at the segment start, so that failure refuses the segment.
    auto const refusedRes = makeSegmentDecoderSession(std::move(refusedPtr), kSegment, 1000);
    REQUIRE_FALSE(refusedRes);
    CHECK(refusedRes.error().code == Error::Code::SeekFailed);

    auto sourcePtr = std::make_unique<FrameSeekDecoderFake>(64);
    auto* const source = sourcePtr.get();
    auto decoderPtr = ao::test::requireValue(makeSegmentDecoderSession(std::move(sourcePtr), kSegment, 1000));
    source->failSeeks(true);
    auto const frameSeekRes = decoderPtr->seekFrame(10);
    REQUIRE_FALSE(frameSeekRes);
    CHECK(frameSeekRes.error().code == Error::Code::SeekFailed);
    auto const timeSeekRes = decoderPtr->seek(std::chrono::milliseconds{5});
    REQUIRE_FALSE(timeSeekRes);
    CHECK(timeSeekRes.error().code == Error::Code::SeekFailed);
    // Neither failure moved the segment, which still plays from its start.
    source->failSeeks(false);
    CHECK(readPcm(*decoderPtr).size() == 40);
  }

  TEST_CASE("SegmentDecoderSession - premature carrier EOF is a stable decode failure",
            "[audio][unit][segment-decoder]")
  {
    auto decoderPtr = ao::test::requireValue(makeSegmentDecoderSession(
      std::make_unique<FrameSeekDecoderFake>(24), {.startFrame = 19, .frameCount = 20, .totalFrames = 64}, 1000));
    auto const res = decoderPtr->readNextBlock();
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::DecodeFailed);
    auto const repeatedRes = decoderPtr->readNextBlock();
    REQUIRE_FALSE(repeatedRes);
    CHECK(repeatedRes.error().code == Error::Code::DecodeFailed);
  }

  TEST_CASE("SegmentDecoderSession - an intact FLAC header with truncated payload fails decoding",
            "[audio][integration][segment-decoder]")
  {
    auto bytes = carrierBytes(true);
    auto const lastFrame =
      buildFlacVerbatimFrameBytes(127, kSampleRate, 2, 16, std::to_array<std::int32_t>({128, -128}));
    bytes.resize(bytes.size() - lastFrame.size());
    auto const temp = ao::test::TempFile{bytes, ".flac"};
    auto decoderPtr = openSegment(temp.path, {.startFrame = 31, .frameCount = 2017, .totalFrames = kCarrierFrames});
    std::uint64_t deliveredFrames = 0;
    bool sawFailure = false;

    for (std::size_t index = 0; index < 256; ++index)
    {
      auto const res = decoderPtr->readNextBlock();

      if (!res)
      {
        CHECK(res.error().code == Error::Code::DecodeFailed);
        sawFailure = true;
        break;
      }

      deliveredFrames += res->frames;
      REQUIRE_FALSE(res->endOfStream);
    }

    CHECK(sawFailure);
    CHECK(deliveredFrames < 2017);
  }

  TEST_CASE("TrackSession - inspection and final preparation enforce stored carrier facts",
            "[audio][integration][segment-decoder]")
  {
    auto const flac = GENERATE(false, true);
    auto const temp = ao::test::TempFile{carrierBytes(flac), flac ? ".flac" : ".wav"};
    auto input =
      PlaybackInput{.filePath = temp.path,
                    .sampleRateHint = kSampleRate,
                    .optSegment = PlaybackSegment{.startFrame = 317, .frameCount = 588, .totalFrames = kCarrierFrames}};
    auto const inspection = ao::test::requireValue(detail::TrackSession::inspect(input, {}));
    CHECK(inspection.info.optTotalFrames == 588);
    auto prepared =
      ao::test::requireValue(detail::TrackSession::prepare(input, inspection, inspection.info.outputFormat, {}));
    auto bytes = std::vector<std::byte>(static_cast<std::size_t>(588) * 4);
    CHECK(prepared.sourcePtr->read(bytes) == bytes.size());
    auto const expected = carrierPcm();
    CHECK(std::ranges::equal(bytes,
                             utility::bytes::view(std::span<std::uint8_t const>{expected})
                               .subspan(static_cast<std::size_t>(317) * 4, static_cast<std::size_t>(588) * 4)));
    CHECK(prepared.sourcePtr->isDrained());

    // The same changed facts must be checked anew by the final decoder, not
    // merely compared with the earlier decoder's inspected signal format.
    SECTION("extent changed")
    {
      ++input.optSegment->totalFrames;
    }

    SECTION("sample rate changed")
    {
      input.sampleRateHint = 48000;
    }

    auto const inspectRes = detail::TrackSession::inspect(input, {});
    REQUIRE_FALSE(inspectRes);
    CHECK(inspectRes.error().code == Error::Code::FormatRejected);
    auto const prepareRes = detail::TrackSession::prepare(input, inspection, inspection.info.outputFormat, {});
    REQUIRE_FALSE(prepareRes);
    CHECK(prepareRes.error().code == Error::Code::FormatRejected);
  }
} // namespace ao::audio::test
