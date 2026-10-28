// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/OpusDecoderSession.h"

#include "DecoderTestSupport.h"
#include "lib/media/ogg/PageLayout.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/media/ogg/TestOgg.h"
#include <ao/AudioCodec.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/media/ogg/Demuxer.h>
#include <ao/media/opus/Header.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    // The mono/stereo fixtures used for exact-extent checks contain one audible second.
    constexpr std::uint64_t kFixtureFrames = 48000;

    void shiftFixtureAudioGranules(std::vector<std::uint8_t>& fileBytes, std::int64_t shift)
    {
      struct PageGranule final
      {
        std::size_t pageOffset = 0;
        std::int64_t granulePosition = 0;
      };

      auto demuxerRes = media::ogg::Demuxer::parse(std::as_bytes(std::span<std::uint8_t const>{fileBytes}));
      REQUIRE(demuxerRes);
      auto const& demuxer = *demuxerRes;
      auto pages = std::vector<PageGranule>{};

      for (std::size_t index = 0; index < demuxer.pageGroupCount(); ++index)
      {
        auto const group = demuxer.pageGroup(index);

        if (group.firstPacketIndex + group.packetCount <= media::opus::kFirstAudioPacketIndex)
        {
          continue;
        }

        // short_pages.opus keeps every completed audio packet on one page, so
        // the packet's start offset is also the page group carrying its granule.
        auto const firstAudioPacketIndex = std::max(group.firstPacketIndex, media::opus::kFirstAudioPacketIndex);
        pages.push_back(PageGranule{
          .pageOffset = demuxer.packetPageOffset(firstAudioPacketIndex), .granulePosition = group.granulePosition});
      }

      REQUIRE_FALSE(pages.empty());
      auto bytes = std::as_writable_bytes(std::span<std::uint8_t>{fileBytes});

      for (auto const& page : pages)
      {
        auto* const header = utility::layout::viewMutable<media::ogg::PageHeaderLayout>(bytes.subspan(page.pageOffset));
        header->granulePosition = page.granulePosition + shift;
      }
    }

    std::vector<std::uint8_t> makeZeroFrameStream()
    {
      auto const source = readFileBytes(requireAudioFixture("basic_metadata.opus"));
      auto const demuxerRes = media::ogg::Demuxer::parse(std::as_bytes(std::span<std::uint8_t const>{source}));
      REQUIRE(demuxerRes);
      REQUIRE(demuxerRes->packetCount() > media::opus::kFirstAudioPacketIndex);

      auto const headRes = media::opus::parseHead(demuxerRes->packet(media::opus::kHeadPacketIndex).bytes);
      REQUIRE(headRes);

      auto const headPacket = ao::test::ogg::toUnsignedBytes(demuxerRes->packet(media::opus::kHeadPacketIndex).bytes);
      auto const tagsPacket = ao::test::ogg::toUnsignedBytes(demuxerRes->packet(media::opus::kTagsPacketIndex).bytes);
      auto const audioPacket =
        ao::test::ogg::toUnsignedBytes(demuxerRes->packet(media::opus::kFirstAudioPacketIndex).bytes);
      auto const pages = std::array{
        ao::test::ogg::Page{.headerType = media::ogg::kBeginOfStreamFlag,
                            .granulePosition = 0,
                            .pageSequence = 0,
                            .lacingValues = ao::test::ogg::lacingFor({headPacket.size()}),
                            .payload = headPacket},
        ao::test::ogg::Page{.granulePosition = 0,
                            .pageSequence = 1,
                            .lacingValues = ao::test::ogg::lacingFor({tagsPacket.size()}),
                            .payload = tagsPacket},
        ao::test::ogg::Page{.headerType = media::ogg::kEndOfStreamFlag,
                            .granulePosition = headRes->preSkip,
                            .pageSequence = 2,
                            .lacingValues = ao::test::ogg::lacingFor({audioPacket.size()}),
                            .payload = audioPacket},
      };
      return ao::test::ogg::toUnsignedBytes(ao::test::ogg::makeStream(pages));
    }

    // Magnitude of one frequency in one channel of an interleaved 16-bit block,
    // by the Goertzel recurrence. Cheaper and clearer here than a full DFT.
    double toneMagnitude(std::span<std::int16_t const> samples,
                         std::uint8_t channels,
                         std::uint8_t channel,
                         double frequency)
    {
      auto const omega = 2.0 * std::numbers::pi * frequency / media::opus::kDecodedSampleRate;
      auto const coefficient = 2.0 * std::cos(omega);
      double previous = 0.0;
      double beforePrevious = 0.0;

      for (std::size_t index = channel; index < samples.size(); index += channels)
      {
        auto const current = samples[index] + (coefficient * previous) - beforePrevious;
        beforePrevious = previous;
        previous = current;
      }

      return std::hypot(previous - (beforePrevious * std::cos(omega)), beforePrevious * std::sin(omega));
    }
  } // namespace

  TEST_CASE("OpusDecoderSession - decodes happy path")
  {
    auto const testFile = requireAudioFixture("basic_metadata.opus");

    auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(testFile, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;

    auto const info = decoder.streamInfo();
    CHECK(info.codec == AudioCodec::Opus);
    CHECK(info.sourceFormat.sampleRate == media::opus::kDecodedSampleRate);
    CHECK(info.sourceFormat.channels == 2);
    CHECK(info.sourceFormat.precisionBits == 16);
    CHECK(info.isLossy);
    CHECK(info.outputFormat.sampleRate == info.sourceFormat.sampleRate);
    CHECK(encodingContainerBits(info.outputFormat.encoding) == 16);
    CHECK(info.duration == std::chrono::seconds{1});

    auto const firstBlockRes = decoder.readNextBlock();
    REQUIRE(firstBlockRes);
    CHECK(firstBlockRes->firstFrameIndex == 0);
    CHECK(firstBlockRes->frames > 0);
    CHECK_FALSE(firstBlockRes->bytes.empty());
  }

  TEST_CASE("OpusDecoderSession - native output encoding probes the decoded stream")
  {
    auto const testFile = requireAudioFixture("basic_metadata.opus");

    auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(testFile, std::nullopt));
    auto& decoder = *decoderPtr;

    auto const info = decoder.streamInfo();
    CHECK(signalFormat(info.outputFormat) == info.sourceFormat);
    CHECK(info.outputFormat.encoding == SampleEncoding::Signed16Le);

    auto const blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    REQUIRE(blockRes->frames > 0);
    CHECK(blockRes->bytes.size() == static_cast<std::size_t>(blockRes->frames) * 2U * 2U);
  }

  TEST_CASE("OpusDecoderSession - reports a mono stream as one channel")
  {
    auto const testFile = requireAudioFixture("mono.opus");

    auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(testFile, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;

    CHECK(decoder.streamInfo().sourceFormat.channels == 1);

    auto const blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    REQUIRE(blockRes->frames > 0);
    CHECK(blockRes->bytes.size() == static_cast<std::size_t>(blockRes->frames) * 1U * 2U);
  }

  TEST_CASE("OpusDecoderSession - handles floating point output")
  {
    auto const testFile = requireAudioFixture("basic_metadata.opus");

    auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(testFile, SampleEncoding::Float32Le));
    auto& decoder = *decoderPtr;

    auto const info = decoder.streamInfo();
    CHECK(isFloatEncoding(info.outputFormat.encoding));

    auto const blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    REQUIRE(blockRes->frames > 0);
    CHECK(blockRes->bytes.size() == static_cast<std::size_t>(blockRes->frames) * 2U * 4U);
  }

  TEST_CASE("OpusDecoderSession - trims the pre-skip and the encoder padding")
  {
    auto const testFile = requireAudioFixture("basic_metadata.opus");

    auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(testFile, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;

    // The stream decodes more than this: the granule position counts a 312
    // sample pre-skip, and its last packet overruns the final granule. Emitting
    // exactly one second proves both ends are trimmed.
    CHECK(readUntilStableEndOfStream(decoder, 512) == kFixtureFrames);
  }

  TEST_CASE("OpusDecoderSession - seeks to an exact playback position")
  {
    auto const testFile = requireAudioFixture("basic_metadata.opus");

    auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(testFile, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;

    SECTION("A mid-stream seek resumes at the requested frame")
    {
      REQUIRE(decoder.seek(std::chrono::milliseconds{500}));

      auto const blockRes = decoder.readNextBlock();
      REQUIRE(blockRes);
      CHECK(blockRes->firstFrameIndex == kFixtureFrames / 2);
      CHECK(blockRes->frames > 0);
    }

    SECTION("The remaining frames after a seek complete the stream")
    {
      REQUIRE(decoder.seek(std::chrono::milliseconds{500}));
      CHECK(readUntilStableEndOfStream(decoder, 512) == kFixtureFrames / 2);
    }

    SECTION("Seeking back to the start replays the whole stream")
    {
      auto const initialBlockRes = decoder.readNextBlock();
      REQUIRE(initialBlockRes);
      REQUIRE(initialBlockRes->frames > 0);
      REQUIRE(decoder.seek(std::chrono::milliseconds{500}));
      REQUIRE(decoder.seek(std::chrono::milliseconds{0}));

      auto const blockRes = decoder.readNextBlock();
      REQUIRE(blockRes);
      CHECK(blockRes->firstFrameIndex == 0);
    }

    SECTION("Seeking past the end reaches a stable end of stream")
    {
      REQUIRE(decoder.seek(std::chrono::seconds{30}));

      auto const blockRes = decoder.readNextBlock();
      REQUIRE(blockRes);
      CHECK(blockRes->endOfStream);
      CHECK(blockRes->bytes.empty());
    }
  }

  TEST_CASE("OpusDecoderSession - nonzero granule origin preserves duration and seek positions")
  {
    constexpr std::int64_t kDecodeOrigin = 24000;
    auto data = readFileBytes(requireAudioFixture("short_pages.opus"));

    // The demuxer deliberately does not verify checksums, so changing only the
    // absolute granule positions produces the cropped/live-join timeline shape
    // without changing any encoded packet.
    shiftFixtureAudioGranules(data, kDecodeOrigin);
    auto const tempFile = ao::test::TempFile{data, ".opus"};

    auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(tempFile.path, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;

    CHECK(decoder.streamInfo().duration == std::chrono::seconds{1});

    SECTION("Sequential decoding emits only the audible duration")
    {
      CHECK(readUntilStableEndOfStream(decoder, 512) == kFixtureFrames);
    }

    SECTION("A mid-stream seek lands on the playback-relative frame")
    {
      REQUIRE(decoder.seek(std::chrono::milliseconds{500}));

      auto const firstBlockRes = decoder.readNextBlock();
      REQUIRE(firstBlockRes);
      CHECK(firstBlockRes->firstFrameIndex == kFixtureFrames / 2);
      CHECK(firstBlockRes->frames > 0);

      auto const remaining =
        static_cast<std::uint64_t>(firstBlockRes->frames) + readUntilStableEndOfStream(decoder, 512);
      CHECK(remaining == kFixtureFrames / 2);
    }
  }

  TEST_CASE("OpusDecoderSession - known zero-frame stream reaches stable end of stream")
  {
    auto const data = makeZeroFrameStream();
    auto const tempFile = ao::test::TempFile{data, ".opus"};

    auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(tempFile.path, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;

    CHECK(decoder.streamInfo().duration == std::chrono::milliseconds{0});

    auto const firstBlockRes = decoder.readNextBlock();
    REQUIRE(firstBlockRes);
    CHECK(firstBlockRes->frames == 0);
    CHECK(firstBlockRes->bytes.empty());
    CHECK(firstBlockRes->endOfStream);

    auto const stableBlockRes = decoder.readNextBlock();
    REQUIRE(stableBlockRes);
    CHECK(stableBlockRes->frames == 0);
    CHECK(stableBlockRes->bytes.empty());
    CHECK(stableBlockRes->endOfStream);
  }

  TEST_CASE("OpusDecoderSession - seek accounting survives a pre-roll walk back")
  {
    // Pages holding a single packet put a seek restart within one packet of its
    // target, so the pre-roll walks back over several pages. The frames it
    // decodes on the way are discarded, which the reported positions prove.
    auto const testFile = requireAudioFixture("short_pages.opus");

    auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(testFile, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;

    SECTION("A mid-stream seek still resumes on the requested frame")
    {
      REQUIRE(decoder.seek(std::chrono::milliseconds{500}));

      auto const blockRes = decoder.readNextBlock();
      REQUIRE(blockRes);
      CHECK(blockRes->firstFrameIndex == kFixtureFrames / 2);
      CHECK(blockRes->frames > 0);
    }

    SECTION("The remaining frames after a seek complete the stream")
    {
      REQUIRE(decoder.seek(std::chrono::milliseconds{500}));
      CHECK(readUntilStableEndOfStream(decoder, 512) == kFixtureFrames / 2);
    }

    SECTION("A seek inside the pre-roll of the stream start replays everything")
    {
      REQUIRE(decoder.seek(std::chrono::milliseconds{10}));

      auto const blockRes = decoder.readNextBlock();
      REQUIRE(blockRes);
      CHECK(blockRes->firstFrameIndex == 480);
    }
  }

  TEST_CASE("OpusDecoderSession - presents family 1 channels in WAV speaker order")
  {
    // libopus emits mapping family 1 in Vorbis channel order, while every
    // positioned PCM surface in Aobus follows WAV speaker order. The
    // fixture puts one tone on each speaker, so the decoded order is provable
    // rather than assumed. Vorbis order would place 300 Hz on channel 1 and
    // 900 Hz on channel 2, which is exactly what this catches.
    constexpr auto kWavOrderTones = std::to_array({600.0, 900.0, 300.0, 60.0, 1500.0, 1800.0});

    auto const testFile = requireAudioFixture("surround.opus");

    auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(testFile, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;

    auto const channels = decoder.streamInfo().sourceFormat.channels;
    REQUIRE(channels == kWavOrderTones.size());

    // Skip the first block so the measurement never lands on the pre-skip edge.
    auto const skippedBlockRes = decoder.readNextBlock();
    REQUIRE(skippedBlockRes);
    REQUIRE(skippedBlockRes->frames > 0);
    auto const blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    REQUIRE(blockRes->frames > 480);

    REQUIRE(blockRes->bytes.size() == static_cast<std::size_t>(blockRes->frames) * channels * 2U);
    auto samples = std::vector<std::int16_t>{};
    samples.reserve(blockRes->bytes.size() / 2U);

    for (std::size_t index = 0; index < blockRes->bytes.size() / 2U; ++index)
    {
      samples.push_back(readSigned16LePcmSample(blockRes->bytes, index));
    }

    for (std::uint8_t channel = 0; channel < channels; ++channel)
    {
      CAPTURE(channel);
      auto const expected = toneMagnitude(samples, channels, channel, kWavOrderTones.at(channel));

      for (std::size_t other = 0; other < kWavOrderTones.size(); ++other)
      {
        if (other == channel)
        {
          continue;
        }

        CAPTURE(kWavOrderTones.at(other));
        CHECK(expected > 10.0 * toneMagnitude(samples, channels, channel, kWavOrderTones.at(other)));
      }
    }
  }

  TEST_CASE("OpusDecoderSession - flush keeps the stream readable")
  {
    auto const testFile = requireAudioFixture("basic_metadata.opus");

    auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(testFile, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;

    auto const initialBlockRes = decoder.readNextBlock();
    REQUIRE(initialBlockRes);
    REQUIRE(initialBlockRes->frames > 0);
    decoder.flush();

    auto const blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    CHECK(blockRes->frames > 0);
  }

  TEST_CASE("OpusDecoderSession - a borrowed block stays byte-stable across seek and flush until the next read")
  {
    // A 500 ms seek at 48 kHz lands on frame 24000.
    constexpr std::uint64_t kSeekTargetFrames = 24000;
    constexpr std::uint8_t kSurroundChannels = 6;

    // The surround fixture's six channels make the borrowed extent nontrivial;
    // S16 and an adapted float output both borrow the same way.
    auto const encodings = std::vector{
      SampleEncoding::Signed16Le,
      SampleEncoding::Float32Le,
    };

    for (std::size_t index = 0; index < encodings.size(); ++index)
    {
      CAPTURE(index);

      auto decoderPtr =
        ao::test::requireValue(OpusDecoderSession::open(requireAudioFixture("surround.opus"), encodings[index]));
      auto& decoder = *decoderPtr;
      REQUIRE(decoder.streamInfo().sourceFormat.channels == kSurroundChannels);

      auto const blockRes = decoder.readNextBlock();
      REQUIRE(blockRes);
      CHECK(blockRes->firstFrameIndex == 0);
      REQUIRE(blockRes->frames > 0);
      REQUIRE_FALSE(blockRes->bytes.empty());

      // The block's bytes alias decoder-owned memory, so this copy is the
      // independent record every later view is compared against.
      auto const borrowed = std::vector<std::byte>{blockRes->bytes.begin(), blockRes->bytes.end()};

      // PcmBlock's bytes stay valid until the next read or close: a seek and a
      // flush do not borrow the buffer again, so the delivered view keeps its
      // bytes across both.
      REQUIRE(decoder.seek(std::chrono::milliseconds{500}));
      CHECK(std::vector<std::byte>{blockRes->bytes.begin(), blockRes->bytes.end()} == borrowed);

      decoder.flush();
      CHECK(std::vector<std::byte>{blockRes->bytes.begin(), blockRes->bytes.end()} == borrowed);

      // The next read is the documented end of the view's validity, so only
      // its metadata is asserted and the old view is not read again.
      auto const nextRes = decoder.readNextBlock();
      REQUIRE(nextRes);
      CHECK(nextRes->frames > 0);
      CHECK(nextRes->firstFrameIndex == kSeekTargetFrames);
      CHECK(nextRes->bytes.size() ==
            frameBytes(decoder.streamInfo().outputFormat) * static_cast<std::size_t>(nextRes->frames));
      CHECK_FALSE(nextRes->endOfStream);
    }
  }
} // namespace ao::audio::test
