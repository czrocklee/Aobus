// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "DecoderTestSupport.h"
#include "lib/audio/DecoderFactory.h"
#include "lib/audio/OpusDecoderSession.h"
#include "lib/media/ogg/PageLayout.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/media/ogg/TestOgg.h"
#include <ao/Error.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/media/ogg/Demuxer.h>
#include <ao/media/opus/Header.h>
#include <ao/media/opus/Timeline.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    constexpr std::uint64_t kFixtureFrames = 48000;
    // surround.opus ends at granule 48238 after pre-skip 312, unlike the stereo fixtures.
    constexpr std::uint64_t kSurroundFrames = 47926;
    // A 500 ms seek at 48 kHz targets frame 24000, independent of any fixture's own extent.
    constexpr std::uint64_t kSeekTargetFrames = 24000;

    // Opus decodes a fixed 48 kHz 16-bit integer signal regardless of family.
    constexpr std::uint8_t kOpusPrecisionBits = 16;

    void checkLayout(DecodedStreamInfo const& info,
                     std::uint8_t channels,
                     std::uint32_t speakerMask,
                     ChannelLayoutOrigin origin,
                     SampleEncoding encoding = SampleEncoding::Signed16Le)
    {
      CHECK(info.sourceFormat == SignalFormat{.sampleRate = media::opus::kDecodedSampleRate,
                                              .channels = channels,
                                              .precisionBits = kOpusPrecisionBits,
                                              .sampleKind = SampleKind::Integer,
                                              .speakerMask = speakerMask});
      CHECK(info.sourceChannelLayout.origin == origin);

      // The output adapter carries the effective speaker identity, so source
      // and output masks agree on every published session.
      CHECK(info.outputFormat == PcmFormat{.sampleRate = media::opus::kDecodedSampleRate,
                                           .channels = channels,
                                           .encoding = encoding,
                                           .speakerMask = speakerMask});
    }

    // Collects the interleaved 16-bit samples of every audible block up to
    // the fixture's independently known extent, so output comparisons are
    // exact and the loop cannot run without progress. The final audible block
    // is followed by the stable empty end-of-stream block.
    std::vector<std::int16_t> collectSamples(DecoderSession& decoder, std::uint64_t expectedFrames = kFixtureFrames)
    {
      auto const openedInfo = decoder.streamInfo();
      auto const channels = openedInfo.sourceFormat.channels;
      auto samples = std::vector<std::int16_t>{};
      std::uint64_t totalFrames = 0;

      while (totalFrames < expectedFrames)
      {
        auto const blockRes = decoder.readNextBlock();
        REQUIRE(blockRes);
        CHECK(decoder.streamInfo() == openedInfo);
        REQUIRE(blockRes->frames > 0);

        totalFrames += blockRes->frames;
        REQUIRE(totalFrames <= expectedFrames);

        auto const count = static_cast<std::size_t>(blockRes->frames) * channels;

        for (std::size_t index = 0; index < count; ++index)
        {
          samples.push_back(readSigned16LePcmSample(blockRes->bytes, index));
        }
      }

      auto const endRes = decoder.readNextBlock();
      REQUIRE(endRes);
      CHECK(decoder.streamInfo() == openedInfo);
      CHECK(endRes->frames == 0);
      CHECK(endRes->bytes.empty());
      CHECK(endRes->endOfStream);
      return samples;
    }

    // One channel's samples, extracted from an interleaved collection.
    std::vector<std::int16_t> channelOf(std::vector<std::int16_t> const& samples,
                                        std::uint8_t channels,
                                        std::uint8_t channel)
    {
      auto result = std::vector<std::int16_t>{};
      result.reserve(samples.size() / channels);

      for (std::size_t frame = 0; frame < samples.size() / channels; ++frame)
      {
        result.push_back(samples[(frame * channels) + channel]);
      }

      return result;
    }

    // A rewritten OpusHead for an explicit mapping family: the channel count,
    // the family, and one mapping entry per output channel, in wire order. The
    // rebuilt packet keeps the fixture's own stream and coupled counts, which
    // the encoded packets already match.
    struct HeadVariant final
    {
      std::uint8_t channels = 0;
      std::uint8_t channelMappingFamily = 0;
      std::vector<std::uint8_t> mappingEntries{};
    };

    // Rebuilds the fixture's OpusHead packet with a variant's channel identity,
    // keeping its version, pre-skip, input rate, and gain untouched.
    std::vector<std::uint8_t> makeHeadPacket(media::opus::Head const& base, HeadVariant const& variant)
    {
      REQUIRE(variant.channelMappingFamily != media::opus::kMappingFamilyMonoStereo);
      REQUIRE(variant.mappingEntries.size() == variant.channels);

      auto packet = std::vector<std::uint8_t>{};

      for (char const character : std::string_view{"OpusHead"})
      {
        packet.push_back(static_cast<std::uint8_t>(character));
      }

      packet.push_back(base.version);
      packet.push_back(variant.channels);
      packet.push_back(static_cast<std::uint8_t>(base.preSkip & 0xFFU));
      packet.push_back(static_cast<std::uint8_t>((base.preSkip >> 8U) & 0xFFU));

      for (std::size_t index = 0; index < sizeof(base.inputSampleRate); ++index)
      {
        packet.push_back(static_cast<std::uint8_t>((base.inputSampleRate >> (index * 8U)) & 0xFFU));
      }

      auto const gain = static_cast<std::uint16_t>(base.outputGain);
      packet.push_back(static_cast<std::uint8_t>(gain & 0xFFU));
      packet.push_back(static_cast<std::uint8_t>((gain >> 8U) & 0xFFU));
      packet.push_back(variant.channelMappingFamily);
      packet.push_back(base.streamCount);
      packet.push_back(base.coupledStreamCount);
      packet.insert(packet.end(), variant.mappingEntries.begin(), variant.mappingEntries.end());
      return packet;
    }

    // One fixture read and parsed once. The object owns the file bytes that
    // every demuxer packet span borrows, so a caller can build head variants
    // and remuxes from one parse while its local fixture lives. It is not a
    // cache and never hands out a demuxer that outlives its bytes.
    struct ParsedOpusFixture final
    {
      ParsedOpusFixture(std::vector<std::uint8_t> ownedBytes,
                        media::ogg::Demuxer parsedDemuxer,
                        media::opus::Head parsedHead)
        : bytes{std::move(ownedBytes)}, demuxer{std::move(parsedDemuxer)}, head{parsedHead}
      {
      }

      ~ParsedOpusFixture() = default;

      ParsedOpusFixture(ParsedOpusFixture const&) = delete;
      ParsedOpusFixture& operator=(ParsedOpusFixture const&) = delete;
      ParsedOpusFixture(ParsedOpusFixture&&) = delete;
      ParsedOpusFixture& operator=(ParsedOpusFixture&&) = delete;

      std::vector<std::uint8_t> bytes;
      media::ogg::Demuxer demuxer; // packet spans borrow bytes
      media::opus::Head head;
    };

    // Reads and parses a fixture: the bytes are read and owned before the
    // demuxer borrows them, and the packet count is validated before any
    // packet is read. Every remux below can rely on these invariants.
    ParsedOpusFixture parseOpusFixture(std::string_view fixtureName)
    {
      auto bytes = readFileBytes(requireAudioFixture(fixtureName));
      auto demuxerRes = media::ogg::Demuxer::parse(std::as_bytes(std::span<std::uint8_t const>{bytes}));
      REQUIRE(demuxerRes);
      REQUIRE(demuxerRes->packetCount() > media::opus::kFirstAudioPacketIndex);
      REQUIRE_FALSE(demuxerRes->hasIncompleteTail());

      auto headRes = media::opus::parseHead(demuxerRes->packet(media::opus::kHeadPacketIndex).bytes);
      REQUIRE(headRes);

      return ParsedOpusFixture{std::move(bytes), std::move(*demuxerRes), *headRes};
    }

    // Re-frames the fixture's Ogg stream around a rewritten OpusHead while
    // keeping every encoded audio packet and its decoded timeline, so only the
    // head's channel identity changes. The fixtures this helper accepts start
    // at the decode origin zero; a source whose own page granules disagree
    // with the forward count is rejected by the crosschecks below.
    std::vector<std::uint8_t> remuxWithHead(ParsedOpusFixture const& fixture, HeadVariant const& variant)
    {
      auto const& demuxer = fixture.demuxer;

      auto pages = std::vector<ao::test::ogg::Page>{};
      std::uint32_t sequence = 0;

      auto const appendPage = [&pages, &sequence](
                                std::vector<std::uint8_t> packet, std::uint8_t headerType, std::int64_t granulePosition)
      {
        pages.push_back(ao::test::ogg::Page{.headerType = headerType,
                                            .granulePosition = granulePosition,
                                            .pageSequence = sequence,
                                            .lacingValues = ao::test::ogg::lacingFor({packet.size()}),
                                            .payload = std::move(packet)});
        ++sequence;
      };

      appendPage(makeHeadPacket(fixture.head, variant), media::ogg::kBeginOfStreamFlag, 0);
      appendPage(ao::test::ogg::toUnsignedBytes(demuxer.packet(media::opus::kTagsPacketIndex).bytes), 0, 0);

      // One audio packet per page keeps every rebuilt granule position exact:
      // positions accumulate each packet's decoded frames forward from the
      // decode origin zero, and only the final page keeps the file's own
      // position so the encoder's tail trim is not smeared across earlier
      // pages.
      auto const audioPackets = demuxer.packetCount() - media::opus::kFirstAudioPacketIndex;
      auto granules = std::vector<std::int64_t>(audioPackets, 0);
      std::int64_t position = 0;

      for (std::size_t index = 0; index < audioPackets; ++index)
      {
        auto const packet = demuxer.packet(media::opus::kFirstAudioPacketIndex + index);
        auto const optFrames = media::opus::packetSampleCount(packet.bytes);
        REQUIRE(optFrames);
        position += *optFrames;
        granules[index] = position;
      }

      auto const finalGranule = demuxer.finalGranulePosition();
      REQUIRE(std::cmp_greater_equal(finalGranule, fixture.head.preSkip));
      REQUIRE(finalGranule <= position);

      // Reframing one packet per page must not move the final page backwards.
      // Equality permits trimming the last packet's entire output.
      if (audioPackets > 1)
      {
        REQUIRE(finalGranule >= granules[audioPackets - 2]);
      }

      granules[audioPackets - 1] = finalGranule;

      for (std::size_t index = 0; index < audioPackets; ++index)
      {
        auto const packet = demuxer.packet(media::opus::kFirstAudioPacketIndex + index);

        // Wherever the fixture completes a page on a packet, the forward count
        // must reproduce the file's own granule, which also rejects a source
        // starting at a nonzero decode origin instead of trusting it.
        if (auto const fileGranule = packet.granulePosition; fileGranule != media::ogg::Demuxer::kUnsetGranulePosition)
        {
          REQUIRE(granules[index] == fileGranule);
        }

        auto const isLast = index + 1 == audioPackets;
        appendPage(ao::test::ogg::toUnsignedBytes(packet.bytes),
                   isLast ? media::ogg::kEndOfStreamFlag : std::uint8_t{0},
                   granules[index]);
      }

      return ao::test::ogg::toUnsignedBytes(ao::test::ogg::makeStream(pages));
    }

    // Parses one fixture and reframes it in a single pass, for callers that
    // remux a fixture only once.
    std::vector<std::uint8_t> remuxWithHead(std::string_view fixtureName, HeadVariant const& variant)
    {
      return remuxWithHead(parseOpusFixture(fixtureName), variant);
    }

    // A family-1 5.1 stream over the surround fixture's own packets whose
    // mapping repeats the back-left coded index in the back-right slot and
    // marks the LFE slot silent. RFC 7845 permits both entries without
    // changing the layout the channel count identifies.
    std::vector<std::uint8_t> makeDuplicateAndSilentSurround()
    {
      auto const fixture = parseOpusFixture("surround.opus");

      auto const& mapping = fixture.head.channelMapping;
      auto const variant =
        HeadVariant{.channels = 6,
                    .channelMappingFamily = media::opus::kMappingFamilySurround,
                    .mappingEntries = {mapping[0], mapping[1], mapping[2], mapping[3], mapping[3], 255}};
      return remuxWithHead(fixture, variant);
    }
  } // namespace

  TEST_CASE("OpusDecoderSession - family 0 identifies mono and stereo layouts")
  {
    SECTION("A family 0 stereo stream identifies FL FR")
    {
      auto const testFile = requireAudioFixture("basic_metadata.opus");

      auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(testFile, SampleEncoding::Signed16Le));
      checkLayout(decoderPtr->streamInfo(), 2, 0x3, ChannelLayoutOrigin::Identified);
    }

    SECTION("A family 0 mono stream identifies FC")
    {
      auto const testFile = requireAudioFixture("mono.opus");

      auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(testFile, SampleEncoding::Signed16Le));
      checkLayout(decoderPtr->streamInfo(), 1, 0x4, ChannelLayoutOrigin::Identified);
    }
  }

  TEST_CASE("OpusDecoderSession - family 1 identifies the WAV layout of every declared channel count")
  {
    // RFC 7845 section 5.1.1.2 names each family 1 output slot after a Vorbis
    // channel role. Each row below declares its slots' speakers and
    // speaker-mask bits literally, in wire order; the lists are this test's
    // own oracle, not a copy of any production permutation table.
    struct WireSpeaker final
    {
      std::string_view name;
      std::uint32_t bit;
    };

    // The mapping family, not the mapping entries, owns the speaker roles, so
    // each row pairs one channel count with the RFC 7845 section 5.1.1 mask.
    // Counts above the stereo fixture's two channels reuse the 5.1 fixture's
    // six decoded stream channels. Every pattern is one legal mapping table:
    // RFC 7845 leaves each entry free to name any coded index or the silent
    // index 255, while the stream and coupled counts stay the fixture's own.
    struct LayoutRow final
    {
      std::string_view fixture;
      std::vector<std::vector<std::uint8_t>> mappingPatterns;
      std::vector<WireSpeaker> wireSpeakers;
      std::uint32_t speakerMask;
      std::uint64_t frames = kFixtureFrames;
    };

    auto const rows = std::vector{
      LayoutRow{.fixture = "mono.opus",
                .mappingPatterns = {{0}},
                .wireSpeakers = {{.name = "FC", .bit = 0x4}},
                .speakerMask = 0x4},
      LayoutRow{.fixture = "short_pages.opus",
                .mappingPatterns = {{0, 1}},
                .wireSpeakers = {{.name = "FL", .bit = 0x1}, {.name = "FR", .bit = 0x2}},
                .speakerMask = 0x3},
      LayoutRow{.fixture = "surround.opus",
                .mappingPatterns = {{0, 1, 2}},
                .wireSpeakers = {{.name = "FL", .bit = 0x1}, {.name = "FC", .bit = 0x4}, {.name = "FR", .bit = 0x2}},
                .speakerMask = 0x7,
                .frames = kSurroundFrames},
      LayoutRow{.fixture = "surround.opus",
                .mappingPatterns = {{0, 1, 2, 3}},
                .wireSpeakers = {{.name = "FL", .bit = 0x1},
                                 {.name = "FR", .bit = 0x2},
                                 {.name = "BL", .bit = 0x10},
                                 {.name = "BR", .bit = 0x20}},
                .speakerMask = 0x33,
                .frames = kSurroundFrames},
      LayoutRow{.fixture = "surround.opus",
                .mappingPatterns = {{0, 1, 2, 3, 4}},
                .wireSpeakers = {{.name = "FL", .bit = 0x1},
                                 {.name = "FC", .bit = 0x4},
                                 {.name = "FR", .bit = 0x2},
                                 {.name = "BL", .bit = 0x10},
                                 {.name = "BR", .bit = 0x20}},
                .speakerMask = 0x37,
                .frames = kSurroundFrames},
      LayoutRow{.fixture = "surround.opus",
                .mappingPatterns = {{0, 1, 2, 3, 4, 5}},
                .wireSpeakers = {{.name = "FL", .bit = 0x1},
                                 {.name = "FC", .bit = 0x4},
                                 {.name = "FR", .bit = 0x2},
                                 {.name = "BL", .bit = 0x10},
                                 {.name = "BR", .bit = 0x20},
                                 {.name = "LFE", .bit = 0x8}},
                .speakerMask = 0x3F,
                .frames = kSurroundFrames},
      LayoutRow{.fixture = "surround.opus",
                // The silent extra slot, then a coded repeat that keeps the
                // extra slot distinguishable from every coded slot.
                .mappingPatterns = {{0, 1, 2, 3, 4, 5, 255}, {0, 1, 2, 3, 4, 5, 0}},
                .wireSpeakers = {{.name = "FL", .bit = 0x1},
                                 {.name = "FC", .bit = 0x4},
                                 {.name = "FR", .bit = 0x2},
                                 {.name = "SL", .bit = 0x200},
                                 {.name = "SR", .bit = 0x400},
                                 {.name = "BC", .bit = 0x100},
                                 {.name = "LFE", .bit = 0x8}},
                .speakerMask = 0x70F,
                .frames = kSurroundFrames},
      LayoutRow{.fixture = "surround.opus",
                // The silent extra slots, then coded repeats that keep the two
                // extra slots distinguishable from each other, so a
                // permutation that confuses them cannot pass both patterns.
                .mappingPatterns = {{0, 1, 2, 3, 4, 5, 255, 255}, {0, 1, 2, 3, 4, 5, 0, 1}},
                .wireSpeakers = {{.name = "FL", .bit = 0x1},
                                 {.name = "FC", .bit = 0x4},
                                 {.name = "FR", .bit = 0x2},
                                 {.name = "SL", .bit = 0x200},
                                 {.name = "SR", .bit = 0x400},
                                 {.name = "BL", .bit = 0x10},
                                 {.name = "BR", .bit = 0x20},
                                 {.name = "LFE", .bit = 0x8}},
                .speakerMask = 0x63F,
                .frames = kSurroundFrames},
    };

    for (auto const& row : rows)
    {
      CAPTURE(row.fixture, row.wireSpeakers.size());

      auto const channels = static_cast<std::uint8_t>(row.wireSpeakers.size());

      // One parse serves every pattern and both mapping families of this row.
      auto const fixture = parseOpusFixture(row.fixture);

      // Sorting the declared speakers by mask bit gives the WAV channel
      // order; locating each of them back in the wire list below gives the
      // family 255 slot that carries the same coded stream.
      auto waveOrder = row.wireSpeakers;
      std::ranges::sort(waveOrder, {}, &WireSpeaker::bit);

      auto const decodeVariant = [&](HeadVariant const& variant, ChannelLayoutOrigin origin, std::uint32_t speakerMask)
      {
        auto const tempFile = ao::test::TempFile{remuxWithHead(fixture, variant), ".opus"};
        auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(tempFile.path, SampleEncoding::Signed16Le));
        auto const info = decoderPtr->streamInfo();
        checkLayout(info, channels, speakerMask, origin);

        // collectSamples pins the row's exact frame extent and the stable
        // empty end-of-stream block behind every decode compared below.
        auto const samples = collectSamples(*decoderPtr, row.frames);
        REQUIRE(samples.size() == row.frames * channels);

        // Reading the whole stream leaves the published info unchanged.
        CHECK(decoderPtr->streamInfo() == info);
        return samples;
      };

      for (std::size_t patternIndex = 0; patternIndex < row.mappingPatterns.size(); ++patternIndex)
      {
        CAPTURE(patternIndex);
        auto const& entries = row.mappingPatterns[patternIndex];

        // The same entries decode twice: family 1 publishes the identified
        // WAVE order, while family 255 assigns no speakers and keeps the
        // head's own slot order, so its bytes are the native ordinal
        // reference for where every coded stream landed.
        auto const identified = decodeVariant(HeadVariant{.channels = channels,
                                                          .channelMappingFamily = media::opus::kMappingFamilySurround,
                                                          .mappingEntries = entries},
                                              ChannelLayoutOrigin::Identified,
                                              row.speakerMask);

        auto const ordinal = decodeVariant(HeadVariant{.channels = channels,
                                                       .channelMappingFamily = media::opus::kMappingFamilyDiscrete,
                                                       .mappingEntries = entries},
                                           ChannelLayoutOrigin::Unknown,
                                           0);

        for (std::uint8_t waveChannel = 0; waveChannel < channels; ++waveChannel)
        {
          auto const wireMatch = std::ranges::find(row.wireSpeakers, waveOrder[waveChannel].bit, &WireSpeaker::bit);
          REQUIRE(wireMatch != row.wireSpeakers.end());
          auto const wireChannel = static_cast<std::uint8_t>(wireMatch - row.wireSpeakers.begin());

          CAPTURE(waveOrder[waveChannel].name);
          CHECK(channelOf(identified, channels, waveChannel) == channelOf(ordinal, channels, wireChannel));
        }
      }
    }
  }

  TEST_CASE("OpusDecoderSession - family 255 keeps a zero mask and unknown origin in header order")
  {
    // The family 0 stereo fixture decodes the same packets any family 255 head
    // maps, because family 255 neither assigns speakers nor reorders slots.
    // Its decoded bytes are therefore the reference each rewritten mapping is
    // compared against.
    auto const original = [&]
    {
      auto const testFile = requireAudioFixture("short_pages.opus");
      auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(testFile, SampleEncoding::Signed16Le));
      auto samples = collectSamples(*decoderPtr);
      REQUIRE(samples.size() == kFixtureFrames * 2U);
      return samples;
    }();

    auto const fixture = parseOpusFixture("short_pages.opus");

    auto const openDiscrete = [&fixture](HeadVariant const& variant)
    {
      auto const tempFile = ao::test::TempFile{remuxWithHead(fixture, variant), ".opus"};
      auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(tempFile.path, SampleEncoding::Signed16Le));
      checkLayout(decoderPtr->streamInfo(), 2, 0, ChannelLayoutOrigin::Unknown);
      return collectSamples(*decoderPtr);
    };

    SECTION("An identity mapping retains the header-defined slot order")
    {
      auto const variant = HeadVariant{
        .channels = 2, .channelMappingFamily = media::opus::kMappingFamilyDiscrete, .mappingEntries = {0, 1}};
      CHECK(openDiscrete(variant) == original);
    }

    SECTION("A duplicate mapping entry repeats one coded index in both slots")
    {
      auto const variant = HeadVariant{
        .channels = 2, .channelMappingFamily = media::opus::kMappingFamilyDiscrete, .mappingEntries = {0, 0}};

      // Both output slots carry the first coded channel, because identities
      // describe output slots, not uniqueness of coded indices.
      auto expected = std::vector<std::int16_t>{};
      expected.reserve(original.size());

      for (std::size_t frame = 0; frame < original.size() / 2U; ++frame)
      {
        expected.push_back(original[(frame * 2U)]);
        expected.push_back(original[(frame * 2U)]);
      }

      CHECK(openDiscrete(variant) == expected);
    }

    SECTION("A silent mapping entry decodes its slot to silence")
    {
      auto const variant = HeadVariant{
        .channels = 2, .channelMappingFamily = media::opus::kMappingFamilyDiscrete, .mappingEntries = {0, 255}};
      auto const samples = openDiscrete(variant);
      REQUIRE(samples.size() == original.size());

      for (std::size_t frame = 0; frame < samples.size() / 2U; ++frame)
      {
        // The mapped slot keeps the original channel, and the silent (255)
        // slot decodes to silence under the Opus mapping API.
        CHECK(samples[(frame * 2U)] == original[(frame * 2U)]);
        CHECK(samples[(frame * 2U) + 1U] == 0);
      }
    }
  }

  TEST_CASE("OpusDecoderSession - a six channel family 255 stream keeps its header slot order")
  {
    // The surround fixture's own family 1 head carries a mapping table, and
    // reusing its entries under family 255 changes only the family: both
    // decodes below draw the same coded streams into the same wire slots.
    // Family 255 assigns no speaker roles; the role lists further down are
    // byte-oracle bookkeeping for this construction, not a claim about the
    // discrete stream's channels.
    auto const fixture = parseOpusFixture("surround.opus");
    REQUIRE(fixture.head.channels == 6);

    auto const& mapping = fixture.head.channelMapping;
    auto const variant =
      HeadVariant{.channels = 6,
                  .channelMappingFamily = media::opus::kMappingFamilyDiscrete,
                  .mappingEntries = {mapping[0], mapping[1], mapping[2], mapping[3], mapping[4], mapping[5]}};

    // The original identifies as 5.1 and publishes WAVE order, so its decoded
    // bytes are the reference the rewrite is compared against.
    auto const wave = [&]
    {
      auto decoderPtr = ao::test::requireValue(
        OpusDecoderSession::open(requireAudioFixture("surround.opus"), SampleEncoding::Signed16Le));
      auto const info = decoderPtr->streamInfo();
      checkLayout(info, 6, 0x3F, ChannelLayoutOrigin::Identified);
      auto samples = collectSamples(*decoderPtr, kSurroundFrames);
      REQUIRE(samples.size() == kSurroundFrames * 6U);
      CHECK(decoderPtr->streamInfo() == info);
      return samples;
    }();

    // The family 255 rewrite publishes the head's own slot order instead.
    auto const ordinal = [&]
    {
      auto const tempFile = ao::test::TempFile{remuxWithHead(fixture, variant), ".opus"};
      auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(tempFile.path, SampleEncoding::Signed16Le));
      auto const info = decoderPtr->streamInfo();
      checkLayout(info, 6, 0, ChannelLayoutOrigin::Unknown);
      auto samples = collectSamples(*decoderPtr, kSurroundFrames);
      REQUIRE(samples.size() == kSurroundFrames * 6U);
      CHECK(decoderPtr->streamInfo() == info);
      return samples;
    }();

    // RFC 7845's family 1 table for six channels names the wire order the
    // fixture's own head uses, and WAVE order sorts those speakers by their
    // mask bits. Locating each WAVE speaker in the wire list names the
    // family 255 slot that must carry the identified channel's bytes, so a
    // family 255 stream wrongly permuted like family 1 cannot match.
    auto const wireRoles = std::vector<std::string_view>{"FL", "FC", "FR", "BL", "BR", "LFE"};
    auto const waveRoles = std::vector<std::string_view>{"FL", "FR", "FC", "LFE", "BL", "BR"};

    for (std::uint8_t waveChannel = 0; waveChannel < 6U; ++waveChannel)
    {
      auto const wireMatch = std::ranges::find(wireRoles, waveRoles[waveChannel]);
      REQUIRE(wireMatch != wireRoles.end());
      auto const wireChannel = static_cast<std::uint8_t>(wireMatch - wireRoles.begin());

      CAPTURE(waveRoles[waveChannel]);
      CHECK(channelOf(ordinal, 6, wireChannel) == channelOf(wave, 6, waveChannel));
    }
  }

  TEST_CASE("OpusDecoderSession - an unsupported mapping family is refused by the concrete session and the factory")
  {
    // This reader supports families 0, 1, and 255; other defined families
    // remain unsupported rather than being guessed from channel count.
    constexpr std::uint8_t kUnsupportedMappingFamily = 2;

    auto const variant =
      HeadVariant{.channels = 2, .channelMappingFamily = kUnsupportedMappingFamily, .mappingEntries = {0, 1}};
    auto const tempFile = ao::test::TempFile{remuxWithHead("short_pages.opus", variant), ".opus"};

    auto const concreteRes = OpusDecoderSession::open(tempFile.path, SampleEncoding::Signed16Le);
    REQUIRE_FALSE(concreteRes);
    CHECK(concreteRes.error().code == Error::Code::NotSupported);

    auto const factoryRes = openDecoderSession(tempFile.path, SampleEncoding::Signed16Le);
    REQUIRE_FALSE(factoryRes);
    CHECK(factoryRes.error().code == Error::Code::NotSupported);
  }

  TEST_CASE("OpusDecoderSession - duplicate and silent mapping entries keep the identified family 1 layout")
  {
    auto const tempFile = ao::test::TempFile{makeDuplicateAndSilentSurround(), ".opus"};

    auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(tempFile.path, SampleEncoding::Signed16Le));

    // Legal mapping entries change no output slot's identity: the stream stays
    // the 5.1 layout its channel count identifies.
    checkLayout(decoderPtr->streamInfo(), 6, 0x3F, ChannelLayoutOrigin::Identified);
    auto const remapped = collectSamples(*decoderPtr, kSurroundFrames);
    REQUIRE(remapped.size() == kSurroundFrames * 6U);

    auto const original = [&]
    {
      auto originalPtr = ao::test::requireValue(
        OpusDecoderSession::open(requireAudioFixture("surround.opus"), SampleEncoding::Signed16Le));
      auto samples = collectSamples(*originalPtr, kSurroundFrames);
      REQUIRE(samples.size() == kSurroundFrames * 6U);
      return samples;
    }();

    auto const channel = [](std::vector<std::int16_t> const& samples, std::uint8_t index)
    { return channelOf(samples, 6, index); };

    // The rewritten mapping keeps every slot's coded index except back-right,
    // which now repeats back-left's, so those slots' PCM is retained exactly.
    CHECK(channel(remapped, 0) == channel(original, 0)); // FL
    CHECK(channel(remapped, 1) == channel(original, 1)); // FR
    CHECK(channel(remapped, 2) == channel(original, 2)); // FC
    CHECK(channel(remapped, 4) == channel(original, 4)); // BL

    // The duplicated back-right slot is byte-identical to back-left, not
    // merely similar.
    CHECK(channel(remapped, 5) == channel(remapped, 4));

    // The silent (255) LFE slot decodes to silence under the Opus mapping API.
    CHECK(std::ranges::all_of(channel(remapped, 3), [](std::int16_t sample) { return sample == 0; }));
  }

  TEST_CASE("OpusDecoderSession - an identified family 1 layout survives seek, flush, and end of stream")
  {
    auto const tempFile = ao::test::TempFile{makeDuplicateAndSilentSurround(), ".opus"};

    auto const openStream = [&tempFile]
    {
      auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(tempFile.path, SampleEncoding::Signed16Le));
      checkLayout(decoderPtr->streamInfo(), 6, 0x3F, ChannelLayoutOrigin::Identified);
      return decoderPtr;
    };

    SECTION("Sequential decoding ends at a stable end of stream")
    {
      auto decoderPtr = openStream();
      auto const openedInfo = decoderPtr->streamInfo();

      CHECK(readUntilStableEndOfStream(*decoderPtr, 512) == kSurroundFrames);
      CHECK(decoderPtr->streamInfo() == openedInfo);
    }

    SECTION("A mid-stream seek lands on the requested frame")
    {
      auto decoderPtr = openStream();
      auto const openedInfo = decoderPtr->streamInfo();

      REQUIRE(decoderPtr->seek(std::chrono::milliseconds{500}));
      CHECK(decoderPtr->streamInfo() == openedInfo);

      auto const blockRes = decoderPtr->readNextBlock();
      REQUIRE(blockRes);
      CHECK(blockRes->firstFrameIndex == kSeekTargetFrames);
      CHECK(blockRes->frames > 0);

      auto const remaining =
        static_cast<std::uint64_t>(blockRes->frames) + readUntilStableEndOfStream(*decoderPtr, 512);
      CHECK(remaining == kSurroundFrames - kSeekTargetFrames);
      CHECK(decoderPtr->streamInfo() == openedInfo);
    }

    SECTION("Flushing mid-stream keeps the session readable")
    {
      auto decoderPtr = openStream();
      auto const openedInfo = decoderPtr->streamInfo();

      auto const initialBlockRes = decoderPtr->readNextBlock();
      REQUIRE(initialBlockRes);
      REQUIRE(initialBlockRes->frames > 0);
      decoderPtr->flush();
      CHECK(decoderPtr->streamInfo() == openedInfo);

      auto const blockRes = decoderPtr->readNextBlock();
      REQUIRE(blockRes);
      CHECK(blockRes->frames > 0);
      CHECK(decoderPtr->streamInfo() == openedInfo);
    }
  }

  TEST_CASE("OpusDecoderSession - factory and concrete opens agree on the published layout")
  {
    struct ParityRow final
    {
      std::string_view fixture;
      std::uint8_t channels = 0;
      std::uint32_t speakerMask = 0;
    };

    auto const rows = std::vector{
      ParityRow{.fixture = "basic_metadata.opus", .channels = 2, .speakerMask = 0x3},
      ParityRow{.fixture = "surround.opus", .channels = 6, .speakerMask = 0x3F},
    };

    for (auto const& row : rows)
    {
      CAPTURE(row.fixture);

      auto const testFile = requireAudioFixture(row.fixture);
      auto concretePtr = ao::test::requireValue(OpusDecoderSession::open(testFile, std::nullopt));
      auto factoryPtr = ao::test::requireValue(openDecoderSession(testFile, std::nullopt));

      checkLayout(concretePtr->streamInfo(), row.channels, row.speakerMask, ChannelLayoutOrigin::Identified);
      CHECK(factoryPtr->streamInfo() == concretePtr->streamInfo());

      auto const concreteRes = concretePtr->readNextBlock();
      auto const factoryRes = factoryPtr->readNextBlock();
      REQUIRE(concreteRes);
      REQUIRE(factoryRes);
      CHECK(factoryRes->firstFrameIndex == concreteRes->firstFrameIndex);
      CHECK(factoryRes->frames == concreteRes->frames);
      REQUIRE(factoryRes->bytes.size() == concreteRes->bytes.size());
      CHECK(std::equal(factoryRes->bytes.begin(), factoryRes->bytes.end(), concreteRes->bytes.begin()));
    }
  }

  TEST_CASE("OpusDecoderSession - the identified layout survives output adaptation")
  {
    auto const testFile = requireAudioFixture("surround.opus");
    auto const encodings = std::vector{
      SampleEncoding::Signed16Le,
      SampleEncoding::Signed32Le,
      SampleEncoding::Float32Le,
    };

    for (std::size_t index = 0; index < encodings.size(); ++index)
    {
      CAPTURE(index);

      auto decoderPtr = ao::test::requireValue(OpusDecoderSession::open(testFile, encodings[index]));
      auto const info = decoderPtr->streamInfo();

      // The helper asserts the complete published pair for the requested
      // encoding, including the unchanged 16-bit integer source format.
      checkLayout(info, 6, 0x3F, ChannelLayoutOrigin::Identified, encodings[index]);

      auto const blockRes = decoderPtr->readNextBlock();
      REQUIRE(blockRes);
      REQUIRE(blockRes->frames > 0);
      CHECK(blockRes->bytes.size() == frameBytes(info.outputFormat) * static_cast<std::size_t>(blockRes->frames));
      CHECK(decoderPtr->streamInfo() == info);
    }
  }
} // namespace ao::audio::test
