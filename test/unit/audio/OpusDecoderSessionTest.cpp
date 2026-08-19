// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/OpusDecoderSession.h"

#include "DecoderTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/media/ogg/TestOgg.h"
#include <ao/AudioCodec.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/media/ogg/Demuxer.h>
#include <ao/media/ogg/PageLayout.h>
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
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    // Every Opus fixture holds exactly one second of audio past its pre-skip.
    constexpr std::uint64_t kFixtureFrames = 48000;

    std::vector<std::uint8_t> toUnsignedBytes(std::span<std::byte const> bytes)
    {
      auto result = std::vector<std::uint8_t>{};
      result.reserve(bytes.size());

      for (auto const byte : bytes)
      {
        result.push_back(std::to_integer<std::uint8_t>(byte));
      }

      return result;
    }

    void shiftFixtureAudioGranules(std::vector<std::uint8_t>& fileBytes, std::int64_t shift)
    {
      struct PageGranule final
      {
        std::size_t pageOffset = 0;
        std::int64_t granulePosition = 0;
      };

      auto demuxerRes = media::ogg::Demuxer::parse(std::as_bytes(std::span{fileBytes}));
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
      auto bytes = std::as_writable_bytes(std::span{fileBytes});

      for (auto const& page : pages)
      {
        auto* const header = utility::layout::viewMutable<media::ogg::PageHeaderLayout>(bytes.subspan(page.pageOffset));
        header->granulePosition = page.granulePosition + shift;
      }
    }

    std::vector<std::uint8_t> makeZeroFrameStream()
    {
      auto const source = readFileBytes(requireAudioFixture("basic_metadata.opus"));
      auto const demuxerRes = media::ogg::Demuxer::parse(std::as_bytes(std::span{source}));
      REQUIRE(demuxerRes);
      REQUIRE(demuxerRes->packetCount() > media::opus::kFirstAudioPacketIndex);

      auto const headRes = media::opus::parseHead(demuxerRes->packet(media::opus::kHeadPacketIndex).bytes);
      REQUIRE(headRes);

      auto const headPacket = toUnsignedBytes(demuxerRes->packet(media::opus::kHeadPacketIndex).bytes);
      auto const tagsPacket = toUnsignedBytes(demuxerRes->packet(media::opus::kTagsPacketIndex).bytes);
      auto const audioPacket = toUnsignedBytes(demuxerRes->packet(media::opus::kFirstAudioPacketIndex).bytes);
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
      return toUnsignedBytes(ao::test::ogg::makeStream(pages));
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

  TEST_CASE("OpusDecoderSession - decodes happy path", "[audio][unit][opus]")
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

  TEST_CASE("OpusDecoderSession - native output encoding probes the decoded stream", "[audio][unit][opus]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.opus");

    auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(testFile, std::nullopt));
    auto& decoder = *decoderPtr;

    auto const info = decoder.streamInfo();
    CHECK(signalFormat(info.outputFormat) == info.sourceFormat);
    CHECK(info.outputFormat.encoding == SampleEncoding::Signed16Le);

    auto const blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    CHECK(blockRes->bytes.size() == static_cast<std::size_t>(blockRes->frames) * 2U * 2U);
  }

  TEST_CASE("OpusDecoderSession - reports a mono stream as one channel", "[audio][unit][opus]")
  {
    auto const testFile = requireAudioFixture("mono.opus");

    auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(testFile, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;

    CHECK(decoder.streamInfo().sourceFormat.channels == 1);

    auto const blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    CHECK(blockRes->bytes.size() == static_cast<std::size_t>(blockRes->frames) * 1U * 2U);
  }

  TEST_CASE("OpusDecoderSession - handles floating point output", "[audio][unit][opus]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.opus");

    auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(testFile, SampleEncoding::Float32Le));
    auto& decoder = *decoderPtr;

    auto const info = decoder.streamInfo();
    CHECK(isFloatEncoding(info.outputFormat.encoding));

    auto const blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    CHECK(blockRes->bytes.size() == static_cast<std::size_t>(blockRes->frames) * 2U * 4U);
  }

  TEST_CASE("OpusDecoderSession - trims the pre-skip and the encoder padding", "[audio][unit][opus]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.opus");

    auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(testFile, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;

    // The stream decodes more than this: the granule position counts a 312
    // sample pre-skip, and its last packet overruns the final granule. Emitting
    // exactly one second proves both ends are trimmed.
    CHECK(readUntilStableEndOfStream(decoder, 512) == kFixtureFrames);
  }

  TEST_CASE("OpusDecoderSession - seeks to an exact playback position", "[audio][unit][opus]")
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
      std::ignore = decoder.readNextBlock();
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

  TEST_CASE("OpusDecoderSession - nonzero granule origin preserves duration and seek positions",
            "[audio][regression][opus]")
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

  TEST_CASE("OpusDecoderSession - known zero-frame stream reaches stable end of stream", "[audio][regression][opus]")
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

  TEST_CASE("OpusDecoderSession - seek accounting survives a pre-roll walk back", "[audio][unit][opus]")
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

  TEST_CASE("OpusDecoderSession - presents family 1 channels in WAV speaker order", "[audio][unit][opus]")
  {
    // libopus emits mapping family 1 in Vorbis channel order, while every PCM
    // surface in Aobus is WAV speaker order and carries no layout metadata. The
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
    std::ignore = decoder.readNextBlock();
    auto const blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    REQUIRE(blockRes->frames > 480);

    auto const samples = std::span{
      reinterpret_cast<std::int16_t const*>(blockRes->bytes.data()), blockRes->bytes.size() / sizeof(std::int16_t)};

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

  TEST_CASE("OpusDecoderSession - flush keeps the stream readable", "[audio][unit][opus]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.opus");

    auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(testFile, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;

    std::ignore = decoder.readNextBlock();
    decoder.flush();

    auto const blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    CHECK(blockRes->frames > 0);
  }
} // namespace ao::audio::test
