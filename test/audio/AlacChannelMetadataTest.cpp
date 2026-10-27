// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AlacFrameTestSupport.h"
#include "lib/audio/AlacDecoderSession.h"
#include "lib/audio/DecoderFactory.h"
#include "test/TestFixtureSupport.h"
#include "test/media/mp4/TestAtoms.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/media/mp4/Atom.h>
#include <ao/media/mp4/Demuxer.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
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
    struct ChannelAtomCase final
    {
      std::string_view name;
      std::vector<std::uint8_t> atom;
    };

    void overwriteBe32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
    {
      REQUIRE(offset + 4U <= bytes.size());
      auto encoded = std::vector<std::uint8_t>{};
      ao::test::mp4::appendBe32(encoded, value);
      std::ranges::copy(encoded, bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    std::size_t alacCookieOffset(std::vector<std::uint8_t> const& bytes)
    {
      auto const fileBytes = utility::bytes::view(std::span<std::uint8_t const>{bytes});
      auto const demuxerRes = media::mp4::Demuxer::parse(fileBytes, "alac");
      REQUIRE(demuxerRes);
      auto const optEntry = demuxerRes->alacSampleEntry();
      REQUIRE(optEntry);
      auto cursor = optEntry->children();

      while (true)
      {
        auto const atomRes = cursor.next();
        REQUIRE(atomRes);
        REQUIRE(*atomRes);

        if (auto const& atom = **atomRes; atom.type() == "alac")
        {
          REQUIRE(atom.bytes().size() >= 36);
          return static_cast<std::size_t>(atom.bytes().data() - fileBytes.data());
        }
      }
    }

    void checkMetadataSession(DecoderSession& decoder,
                              AlacFrameTestOptions const& options,
                              std::uint32_t mask,
                              std::span<std::uint8_t const> expectedFrame)
    {
      // The role decision is observable on the ready session, not after a read.
      auto const info = decoder.streamInfo();
      CHECK(info.codec == AudioCodec::Alac);
      CHECK_FALSE(info.isLossy);
      CHECK(info.sourceFormat.channels == options.channels);
      CHECK(info.outputFormat.channels == options.channels);
      CHECK(info.sourceFormat.sampleRate == 48000);
      CHECK(info.outputFormat.sampleRate == 48000);
      CHECK(info.sourceFormat.precisionBits == 16);
      CHECK(info.sourceFormat.sampleKind == SampleKind::Integer);
      CHECK(info.outputFormat.encoding == SampleEncoding::Signed16Le);
      CHECK(info.sourceFormat.speakerMask == mask);
      CHECK(info.outputFormat.speakerMask == mask);
      CHECK(info.sourceChannelLayout.origin ==
            (mask == 0 ? ChannelLayoutOrigin::Unknown : ChannelLayoutOrigin::Identified));
      CHECK(signalFormat(info.outputFormat) == info.sourceFormat);
      REQUIRE(expectedFrame.size() == frameBytes(info.outputFormat));
      auto expectedPacket = std::vector<std::uint8_t>{};

      for (std::uint32_t frameIndex = 0; frameIndex < 16; ++frameIndex)
      {
        expectedPacket.insert(expectedPacket.end(), expectedFrame.begin(), expectedFrame.end());
      }

      for (std::uint32_t packetIndex = 0; packetIndex < options.packetCount; ++packetIndex)
      {
        CAPTURE(packetIndex);
        auto const blockRes = decoder.readNextBlock();
        REQUIRE(blockRes);
        CHECK(blockRes->frames == 16);
        CHECK(blockRes->firstFrameIndex == static_cast<std::uint64_t>(packetIndex) * 16U);
        CHECK(blockRes->endOfStream == (packetIndex + 1U == options.packetCount));
        CHECK(std::ranges::equal(blockRes->bytes, utility::bytes::view(std::span<std::uint8_t const>{expectedPacket})));
        CHECK(decoder.streamInfo() == info);
      }

      for (std::uint32_t repeat = 0; repeat < 2; ++repeat)
      {
        auto const eofRes = decoder.readNextBlock();
        REQUIRE(eofRes);
        CHECK(eofRes->endOfStream);
        CHECK(eofRes->frames == 0);
        CHECK(eofRes->bytes.empty());
        CHECK(decoder.streamInfo() == info);
      }
    }

    void checkMetadataBytes(std::vector<std::uint8_t> const& bytes,
                            AlacFrameTestOptions const& options,
                            std::uint32_t mask = 0,
                            std::span<std::uint8_t const> expectedFrame = {})
    {
      // Literal ordinal S16LE arrival markers; no role/permutation calculation.
      constexpr auto kRawFrame = std::to_array<std::uint8_t>(
        {0x52, 0x51, 0x64, 0x63, 0x76, 0x75, 0xE8, 0xE8, 0x2A, 0x29, 0xC4, 0xC4, 0x4E, 0x4D, 0xA0, 0xA0});

      if (expectedFrame.empty())
      {
        REQUIRE(options.channels <= 8);
        expectedFrame = std::span<std::uint8_t const>{kRawFrame}.first(static_cast<std::size_t>(options.channels) * 2U);
      }

      auto const temp = ao::test::TempFile{bytes, ".m4a"};
      auto concretePtr = ao::test::requireValue(AlacDecoderSession::open(temp.path, std::nullopt));
      auto factoryPtr = ao::test::requireValue(openDecoderSession(temp.path, std::nullopt));
      REQUIRE(concretePtr);
      REQUIRE(factoryPtr);
      CHECK(concretePtr->streamInfo() == factoryPtr->streamInfo());
      checkMetadataSession(*concretePtr, options, mask, expectedFrame);
      checkMetadataSession(*factoryPtr, options, mask, expectedFrame);
    }

    void checkMetadata(AlacFrameTestOptions const& options,
                       std::uint32_t mask = 0,
                       std::span<std::uint8_t const> expectedFrame = {})
    {
      checkMetadataBytes(buildAlacUncompressedFileBytes(options), options, mask, expectedFrame);
    }
  } // namespace

  TEST_CASE("AlacDecoderSession - reads agreeing channel declarations around and inside the selected cookie",
            "[audio][integration][alac][metadata]")
  {
    auto options = AlacFrameTestOptions{.channels = 4, .packetCount = 2};
    auto const layout = makeAlacChannelLayoutAtom(0x00740004);
    auto const canonical = std::to_array<std::uint8_t>({0x64, 0x63, 0x76, 0x75, 0x52, 0x51, 0xE8, 0xE8});

    SECTION("Sibling before cookie")
    {
      options.sampleEntryPrefix = layout;
    }

    SECTION("Sibling after cookie")
    {
      options.sampleEntrySuffix = layout;
    }

    SECTION("Extended sibling header")
    {
      options.sampleEntrySuffix = ao::test::mp4::makeExtendedFromCompactAtom(layout);
    }

    SECTION("Compact cookie-contained named declaration")
    {
      options.cookieSuffix = layout;
    }

    SECTION("Duplicate siblings agree on ordered slots")
    {
      options.sampleEntryPrefix = layout;
      options.sampleEntrySuffix = layout;
    }

    SECTION("Cookie and both siblings agree")
    {
      options.sampleEntryPrefix = layout;
      options.cookieSuffix = layout;
      options.sampleEntrySuffix = layout;
    }

    SECTION("Named and ordered descriptions agree")
    {
      auto const labels = std::to_array<std::uint32_t>({3, 1, 2, 9});
      options.cookieSuffix = layout;
      options.sampleEntrySuffix = makeAlacChannelLayoutAtom(0, 0, labels);
    }

    SECTION("Unrelated well-formed optional children do not supply or erase roles")
    {
      options.sampleEntryPrefix = ao::test::mp4::makeAtom("free", {1, 2, 3});
      options.cookieSuffix = layout;
      options.sampleEntrySuffix = ao::test::mp4::makeAtom("junk", {4, 5});
    }

    checkMetadata(options, 0x107, canonical);
  }

  TEST_CASE("AlacDecoderSession - compact configuration supplies decoded geometry independently of sample entry fields",
            "[audio][integration][alac][metadata]")
  {
    auto const options = AlacFrameTestOptions{
      .channels = 4, .precisionBits = 16, .packetCount = 2, .sampleEntrySuffix = makeAlacChannelLayoutAtom(0x00740004)};
    auto bytes = buildAlacUncompressedFileBytes(options);
    auto const fileBytes = utility::bytes::view(std::span<std::uint8_t const>{bytes});
    auto const demuxerRes = media::mp4::Demuxer::parse(fileBytes, "alac");
    REQUIRE(demuxerRes);
    auto const optEntry = demuxerRes->alacSampleEntry();
    REQUIRE(optEntry);
    auto const payload = optEntry->payload();
    REQUIRE(payload.size() >= 28);
    REQUIRE(payload[8] == std::byte{0});
    REQUIRE(payload[9] == std::byte{0});
    auto const payloadOffset = static_cast<std::size_t>(payload.data() - fileBytes.data());

    // Change only the version-zero entry's redundant geometry, not the compact
    // ALAC configuration, channel declaration, timing tables, or packet bytes.
    bytes[payloadOffset + 16U] = 0;
    bytes[payloadOffset + 17U] = 2;
    bytes[payloadOffset + 18U] = 0;
    bytes[payloadOffset + 19U] = 8;
    overwriteBe32(bytes, payloadOffset + 24U, 44100U << 16U);

    auto const canonical = std::to_array<std::uint8_t>({0x64, 0x63, 0x76, 0x75, 0x52, 0x51, 0xE8, 0xE8});
    checkMetadataBytes(bytes, options, 0x107, canonical);
  }

  TEST_CASE("AlacDecoderSession - a later required cookie cannot borrow the selected entry channel declaration",
            "[audio][integration][alac][metadata]")
  {
    auto const donor = buildAlacUncompressedFileBytes(4);
    auto const donorRes =
      media::mp4::Demuxer::parse(utility::bytes::view(std::span<std::uint8_t const>{donor}), "alac");
    REQUIRE(donorRes);
    auto packet = std::vector<std::uint8_t>{};
    auto cookie = std::vector<std::uint8_t>{};

    for (auto const byte : donorRes->samplePayload(0))
    {
      packet.push_back(static_cast<std::uint8_t>(byte));
    }

    for (auto const byte : donorRes->magicCookie())
    {
      cookie.push_back(static_cast<std::uint8_t>(byte));
    }

    auto const declaration = makeAlacChannelLayoutAtom(0x00740004);
    auto const firstEntry = ao::test::mp4::makeAudioSampleEntryAtom("alac", declaration);
    cookie.insert(cookie.end(), declaration.begin(), declaration.end());
    auto const laterEntry = ao::test::mp4::makeAudioSampleEntryAtom("alac", cookie);
    auto stsd = std::vector<std::uint8_t>{};

    SECTION("Extra physical entry despite declared count one")
    {
      stsd = ao::test::mp4::makeStsdAtomFromSampleEntries({firstEntry, laterEntry});
      overwriteBe32(stsd, 12, 1);
    }

    SECTION("A later sample description box supplies the cookie")
    {
      stsd = ao::test::mp4::makeStsdAtomFromSampleEntry(firstEntry);
      auto const later = ao::test::mp4::makeStsdAtomFromSampleEntry(laterEntry);
      stsd.insert(stsd.end(), later.begin(), later.end());
    }

    REQUIRE(packet.size() <= 0xFFFFFFFFU);
    auto const table = ao::test::mp4::makeSampleTableAtom(stsd, static_cast<std::uint32_t>(packet.size()), 16, 8);
    auto const track = ao::test::mp4::makeTrackAtom("soun", table, 48000, 16);
    auto file = ao::test::mp4::makeAtom("mdat", packet);
    ao::test::mp4::addAtom(file, "moov", track);
    auto const associatedRes =
      media::mp4::Demuxer::parse(utility::bytes::view(std::span<std::uint8_t const>{file}), "alac");
    REQUIRE(associatedRes);
    CHECK_FALSE(associatedRes->alacSampleEntry());
    // Neither an early matching declaration nor the later cookie's own valid
    // sibling can certify which description supplied the decoded PCM.
    checkMetadataBytes(file, AlacFrameTestOptions{.channels = 4});
  }

  TEST_CASE("AlacDecoderSession - repeated inner cookies withhold optional identity without replacing required data",
            "[audio][integration][alac][metadata]")
  {
    auto const donor = buildAlacUncompressedFileBytes(4);
    auto const donorRes =
      media::mp4::Demuxer::parse(utility::bytes::view(std::span<std::uint8_t const>{donor}), "alac");
    REQUIRE(donorRes);
    auto duplicate = std::vector<std::uint8_t>{};

    for (auto const byte : donorRes->magicCookie())
    {
      duplicate.push_back(static_cast<std::uint8_t>(byte));
    }

    auto const declaration = makeAlacChannelLayoutAtom(0x00740004);
    duplicate.insert(duplicate.end(), declaration.begin(), declaration.end());
    checkMetadata({.channels = 4, .sampleEntryPrefix = declaration, .sampleEntrySuffix = duplicate});
  }

  TEST_CASE("AlacDecoderSession - generic named sibling variants retain their own explicit slot order",
            "[audio][integration][alac][metadata]")
  {
    struct NamedCase final
    {
      std::uint8_t channels;
      std::uint32_t tag;
      std::uint32_t mask;
      std::vector<std::uint8_t> frame;
    };
    // Arrival orders are quoted from Apple's CoreAudioBaseTypes.h, not the decoder table.
    auto const cases = std::vector<NamedCase>{
      // Quadraphonic: L R Ls Rs.
      {4, 0x006C0004, 0x33, {0x52, 0x51, 0x64, 0x63, 0x76, 0x75, 0xE8, 0xE8}},
      // MPEG 4.0 A: L R C Cs.
      {4, 0x00730004, 0x107, {0x52, 0x51, 0x64, 0x63, 0x76, 0x75, 0xE8, 0xE8}},
      // MPEG 5.0 A: L R C Ls Rs.
      {5, 0x00750005, 0x37, {0x52, 0x51, 0x64, 0x63, 0x76, 0x75, 0xE8, 0xE8, 0x2A, 0x29}},
      // MPEG 5.0 B: L R Ls Rs C. Canonical WAVE slots are 0, 1, 4, 2, 3.
      {5, 0x00760005, 0x37, {0x52, 0x51, 0x64, 0x63, 0x2A, 0x29, 0x76, 0x75, 0xE8, 0xE8}},
      // MPEG 5.1 A: L R C LFE Ls Rs.
      {6, 0x00790006, 0x3F, {0x52, 0x51, 0x64, 0x63, 0x76, 0x75, 0xE8, 0xE8, 0x2A, 0x29, 0xC4, 0xC4}},
      // MPEG 5.1 B: L R Ls Rs C LFE.
      {6, 0x007A0006, 0x3F, {0x52, 0x51, 0x64, 0x63, 0x2A, 0x29, 0xC4, 0xC4, 0x76, 0x75, 0xE8, 0xE8}},
      // MPEG 5.1 C: L C R Ls Rs LFE.
      {6, 0x007B0006, 0x3F, {0x52, 0x51, 0x76, 0x75, 0x64, 0x63, 0xC4, 0xC4, 0xE8, 0xE8, 0x2A, 0x29}},
      // MPEG 7.1 A: L R C LFE Ls Rs Lc Rc.
      {8,
       0x007E0008,
       0xFF,
       {0x52, 0x51, 0x64, 0x63, 0x76, 0x75, 0xE8, 0xE8, 0x2A, 0x29, 0xC4, 0xC4, 0x4E, 0x4D, 0xA0, 0xA0}},
    };

    for (auto const& testCase : cases)
    {
      CAPTURE(testCase.tag);
      checkMetadata({.channels = testCase.channels, .sampleEntrySuffix = makeAlacChannelLayoutAtom(testCase.tag)},
                    testCase.mask,
                    testCase.frame);
    }
  }

  TEST_CASE("AlacDecoderSession - named descriptions map all eighteen WAVE positions without rear-side aliases",
            "[audio][integration][alac][metadata]")
  {
    struct LabelCase final
    {
      std::uint32_t label;
      std::uint32_t mask;
    };
    constexpr auto kCases = std::to_array<LabelCase>({
      {1, 0x1},
      {2, 0x2},
      {3, 0x4},
      {4, 0x8},
      {5, 0x10},
      {6, 0x20},
      {7, 0x40},
      {8, 0x80},
      {9, 0x100},
      {10, 0x200},
      {11, 0x400},
      {12, 0x800},
      {13, 0x1000},
      {14, 0x2000},
      {15, 0x4000},
      {16, 0x8000},
      {17, 0x10000},
      {18, 0x20000},
    });

    for (auto const& testCase : kCases)
    {
      CAPTURE(testCase.label, testCase.mask);
      auto const labels = std::to_array<std::uint32_t>({testCase.label});
      checkMetadata({.channels = 1, .sampleEntrySuffix = makeAlacChannelLayoutAtom(0, 0, labels)}, testCase.mask);
    }

    auto const reversed = std::to_array<std::uint8_t>({0x64, 0x63, 0x52, 0x51});
    auto const backLabels = std::to_array<std::uint32_t>({6, 5});
    checkMetadata({.channels = 2, .sampleEntrySuffix = makeAlacChannelLayoutAtom(0, 0, backLabels)}, 0x30, reversed);
    auto const sideLabels = std::to_array<std::uint32_t>({11, 10});
    checkMetadata({.channels = 2, .sampleEntrySuffix = makeAlacChannelLayoutAtom(0, 0, sideLabels)}, 0x600, reversed);
  }

  TEST_CASE("AlacDecoderSession - declarations with the same speaker set must also agree on slot order",
            "[audio][integration][alac][metadata]")
  {
    auto options = AlacFrameTestOptions{.channels = 4, .packetCount = 2};
    auto const centerFirst = makeAlacChannelLayoutAtom(0x00740004);
    auto const leftFirst = makeAlacChannelLayoutAtom(0x00730004);

    SECTION("Left-first sibling then center-first sibling")
    {
      options.sampleEntryPrefix = leftFirst;
      options.sampleEntrySuffix = centerFirst;
    }

    SECTION("Center-first sibling then left-first sibling")
    {
      options.sampleEntryPrefix = centerFirst;
      options.sampleEntrySuffix = leftFirst;
    }

    SECTION("Cookie and sibling disagree at identical mask")
    {
      options.cookieSuffix = centerFirst;
      options.sampleEntrySuffix = leftFirst;
    }

    SECTION("Descriptions disagree with cookie at identical mask")
    {
      auto const labels = std::to_array<std::uint32_t>({1, 2, 3, 9});
      options.cookieSuffix = centerFirst;
      options.sampleEntrySuffix = makeAlacChannelLayoutAtom(0, 0, labels);
    }

    SECTION("A third agreeing declaration cannot repair the conflict")
    {
      options.sampleEntryPrefix = leftFirst;
      options.cookieSuffix = centerFirst;
      options.sampleEntrySuffix = centerFirst;
    }

    SECTION("MPEG 5.0 B and D conflict despite their identical speaker mask")
    {
      // Apple's B order is L R Ls Rs C; D is C L R Ls Rs.
      options.channels = 5;
      options.sampleEntryPrefix = makeAlacChannelLayoutAtom(0x00760005);
      options.sampleEntrySuffix = makeAlacChannelLayoutAtom(0x00780005);
    }

    SECTION("Different speaker sets also conflict")
    {
      options.sampleEntryPrefix = centerFirst;
      options.sampleEntrySuffix = makeAlacChannelLayoutAtom(0x00010000, 0x33);
    }

    checkMetadata(options);
  }

  TEST_CASE("AlacDecoderSession - ambiguous full three-channel tags never acquire a namespace from placement",
            "[audio][integration][alac][metadata]")
  {
    auto options = AlacFrameTestOptions{.channels = 3, .packetCount = 2};
    auto const ambiguous = makeAlacChannelLayoutAtom(0x00710003);
    auto const clear = makeAlacChannelLayoutAtom(0x00720003);

    SECTION("Sibling before cookie")
    {
      options.sampleEntryPrefix = ambiguous;
    }

    SECTION("Sibling after cookie")
    {
      options.sampleEntrySuffix = ambiguous;
    }

    SECTION("Cookie-contained declaration")
    {
      options.cookieSuffix = ambiguous;
    }

    SECTION("A later unambiguous sibling cannot repair it")
    {
      options.sampleEntryPrefix = ambiguous;
      options.sampleEntrySuffix = clear;
    }

    SECTION("A later clear sibling cannot repair ambiguous cookie metadata")
    {
      options.cookieSuffix = ambiguous;
      options.sampleEntrySuffix = clear;
    }

    SECTION("An earlier clear sibling cannot override ambiguity")
    {
      options.sampleEntryPrefix = clear;
      options.sampleEntrySuffix = ambiguous;
    }

    checkMetadata(options);
  }

  TEST_CASE("AlacDecoderSession - accepts only full big-endian named tag values",
            "[audio][integration][alac][metadata]")
  {
    for (auto const tag : std::to_array<std::uint32_t>({0x04007400, 0x00000004, 0xFFFF0004, 0x00740003}))
    {
      CAPTURE(tag);
      checkMetadata({.channels = 4, .sampleEntrySuffix = makeAlacChannelLayoutAtom(tag)});
    }
  }

  TEST_CASE("AlacDecoderSession - bad optional declarations permanently withhold even mono and stereo identity",
            "[audio][integration][alac][metadata]")
  {
    for (auto const channels : std::to_array<std::uint8_t>({1, 2}))
    {
      CAPTURE(channels);
      auto const good = makeAlacChannelLayoutAtom(channels == 1 ? 0x00640001 : 0x00650002);
      auto bad = good;
      bad[11] = 1; // Nonzero FullBox flags, not absence.

      for (bool const badFirst : {false, true})
      {
        CAPTURE(badFirst);
        checkMetadata({.channels = channels,
                       .sampleEntryPrefix = badFirst ? bad : good,
                       .sampleEntrySuffix = badFirst ? good : bad});
      }
    }
  }

  TEST_CASE("AlacDecoderSession - malformed channel payloads are optional failures without later repair",
            "[audio][integration][alac][metadata]")
  {
    auto const labels = std::to_array<std::uint32_t>({3, 1, 2});
    auto const good = makeAlacChannelLayoutAtom(0x00720003);
    auto flags = good;
    flags[11] = 1;
    auto version = good;
    version[8] = 1;
    auto namedBitmap = good;
    overwriteBe32(namedBitmap, 16, 0x7);
    auto namedDescriptions = good;
    overwriteBe32(namedDescriptions, 20, 3);
    auto overdeclared = makeAlacChannelLayoutAtom(0, 0, labels);
    overwriteBe32(overdeclared, 20, 0xFFFFFFFF);
    auto countMismatch = makeAlacChannelLayoutAtom(0, 0, labels);
    overwriteBe32(countMismatch, 20, 2);
    auto descriptions = makeAlacChannelLayoutAtom(0, 0, labels);
    auto truncatedBody = std::vector<std::uint8_t>(descriptions.begin() + 8, descriptions.end() - 1);
    auto extraBody = std::vector<std::uint8_t>(good.begin() + 8, good.end());
    extraBody.push_back(0);
    auto const cases = std::vector<ChannelAtomCase>{
      {"FullBox flags", flags},
      {"FullBox version", version},
      {"Empty payload", ao::test::mp4::makeAtom("chan", {})},
      {"Short fixed payload", ao::test::mp4::makeAtom("chan", std::vector<std::uint8_t>(15, 0))},
      {"Named tag with bitmap", namedBitmap},
      {"Named tag with descriptions", namedDescriptions},
      {"Overdeclared descriptions", overdeclared},
      {"Description count mismatch", countMismatch},
      {"Truncated description", ao::test::mp4::makeAtom("chan", truncatedBody)},
      {"Unexpected named trailing byte", ao::test::mp4::makeAtom("chan", extraBody)},
      {"Named count mismatch", makeAlacChannelLayoutAtom(0x00740004)},
    };

    for (auto const& testCase : cases)
    {
      CAPTURE(testCase.name);
      auto suffix = testCase.atom;
      suffix.insert(suffix.end(), good.begin(), good.end());
      checkMetadata({.channels = 3, .sampleEntrySuffix = suffix});
    }
  }

  TEST_CASE("AlacDecoderSession - incomplete reserved wildcard and mixed bitmaps do not assign roles",
            "[audio][integration][alac][metadata]")
  {
    for (auto const bitmap : std::to_array<std::uint32_t>({0, 0x3, 0xF, 0x40003, 0x80000003, 0xFFFFFFFF}))
    {
      CAPTURE(bitmap);
      checkMetadata({.channels = 3, .sampleEntrySuffix = makeAlacChannelLayoutAtom(0x00010000, bitmap)});
    }

    auto const labels = std::to_array<std::uint32_t>({1, 2, 3});
    checkMetadata({.channels = 3, .sampleEntrySuffix = makeAlacChannelLayoutAtom(0x00010000, 0x7, labels)});
    checkMetadata({.channels = 3, .sampleEntrySuffix = makeAlacChannelLayoutAtom(0, 0x7, labels)});
  }

  TEST_CASE("AlacDecoderSession - repeated unsupported discrete and coordinate descriptions remain ordinal",
            "[audio][integration][alac][metadata]")
  {
    auto const cases = std::vector<std::vector<std::uint32_t>>{
      {1, 1, 2},
      {0, 1, 2},
      {19, 1, 2},
      {33, 1, 2},
      {34, 1, 2},
      {0x00010000, 1, 2},
      {100, 1, 2},
    };

    for (auto const& labels : cases)
    {
      CAPTURE(labels[0]);
      checkMetadata({.channels = 3, .sampleEntrySuffix = makeAlacChannelLayoutAtom(0, 0, labels)});
    }

    auto const labels = std::to_array<std::uint32_t>({3, 1, 2});
    auto flagged = makeAlacChannelLayoutAtom(0, 0, labels);
    overwriteBe32(flagged, 28, 1); // First description's flags.
    checkMetadata({.channels = 3, .sampleEntrySuffix = flagged});

    auto coordinates = flagged;
    overwriteBe32(coordinates, 24, 100);        // UseCoordinates is not a named speaker.
    overwriteBe32(coordinates, 32, 0x3F800000); // x = 1.0f, independent literal IEEE bits.
    checkMetadata({.channels = 3, .sampleEntrySuffix = coordinates});
  }

  TEST_CASE("AlacDecoderSession - cookie-contained channel grammar excludes generic-only declarations",
            "[audio][integration][alac][metadata]")
  {
    auto const labels = std::to_array<std::uint32_t>({3, 1, 2});
    auto const cases = std::vector<ChannelAtomCase>{
      {"Generic named three-channel tag", makeAlacChannelLayoutAtom(0x00720003)},
      {"Generic bitmap", makeAlacChannelLayoutAtom(0x00010000, 0x7)},
      {"Generic descriptions", makeAlacChannelLayoutAtom(0, 0, labels)},
    };

    for (auto const& testCase : cases)
    {
      CAPTURE(testCase.name);
      checkMetadata(
        {.channels = 3, .sampleEntrySuffix = makeAlacChannelLayoutAtom(0x00720003), .cookieSuffix = testCase.atom});
    }

    checkMetadata({.channels = 4, .cookieSuffix = makeAlacChannelLayoutAtom(0x00730004)});
    checkMetadata({.channels = 4,
                   .cookieSuffix = ao::test::mp4::makeExtendedFromCompactAtom(makeAlacChannelLayoutAtom(0x00740004))});
  }

  TEST_CASE("AlacDecoderSession - nonzero reserved cookie layout words cannot be reinterpreted as generic fields",
            "[audio][integration][alac][metadata]")
  {
    for (auto const offset : std::to_array<std::size_t>({16, 20}))
    {
      CAPTURE(offset);
      auto layout = makeAlacChannelLayoutAtom(0x00740004);
      overwriteBe32(layout, offset, offset == 16 ? 0x107 : 4);
      checkMetadata(
        {.channels = 4, .sampleEntrySuffix = makeAlacChannelLayoutAtom(0x00740004), .cookieSuffix = layout});
    }
  }

  TEST_CASE("AlacDecoderSession - nonzero cookie FullBox words withhold optional identity without blocking PCM",
            "[audio][integration][alac][metadata]")
  {
    for (auto const reserved : std::to_array<std::uint32_t>({1, 0x01000000}))
    {
      CAPTURE(reserved);
      auto const options =
        AlacFrameTestOptions{.channels = 4, .sampleEntrySuffix = makeAlacChannelLayoutAtom(0x00740004)};
      auto bytes = buildAlacUncompressedFileBytes(options);
      auto const offset = alacCookieOffset(bytes);
      overwriteBe32(bytes, offset + 8U, reserved);
      checkMetadataBytes(bytes, options);
    }
  }

  TEST_CASE("AlacDecoderSession - malformed optional tails after a valid cookie withhold identity but decode PCM",
            "[audio][integration][alac][metadata]")
  {
    auto oversized = ao::test::mp4::makeAtom("junk", {});
    overwriteBe32(oversized, 0, 64);
    auto zeroChan = makeAlacChannelLayoutAtom(0x00740004);
    overwriteBe32(zeroChan, 0, 0);
    auto zeroFree = ao::test::mp4::makeAtom("free", {});
    overwriteBe32(zeroFree, 0, 0);
    auto const tails = std::vector<ChannelAtomCase>{
      {"Truncated atom header", {0, 0, 0}},
      {"Declared tail exceeds entry", oversized},
      {"Zero-sized channel tail", zeroChan},
      {"Zero-sized unrelated tail", zeroFree},
    };

    for (auto const& tail : tails)
    {
      for (bool const insideCookie : {false, true})
      {
        CAPTURE(tail.name, insideCookie);
        auto options = AlacFrameTestOptions{
          .channels = 4, .packetCount = 2, .sampleEntryPrefix = makeAlacChannelLayoutAtom(0x00740004)};

        if (insideCookie)
        {
          options.cookieSuffix = tail.atom;
        }
        else
        {
          options.sampleEntrySuffix = tail.atom;
        }

        checkMetadata(options);
      }
    }
  }

  TEST_CASE("AlacDecoderSession - malformed required prefixes still fail before a later valid cookie",
            "[audio][integration][alac][metadata]")
  {
    auto malformed = ao::test::mp4::makeAtom("junk", {});
    overwriteBe32(malformed, 0, 0xFFFFFFFF);
    auto zeroSized = ao::test::mp4::makeAtom("junk", {});
    overwriteBe32(zeroSized, 0, 0);

    for (auto const& prefix : std::vector{malformed, zeroSized})
    {
      auto const options = AlacFrameTestOptions{
        .channels = 4, .sampleEntryPrefix = prefix, .sampleEntrySuffix = makeAlacChannelLayoutAtom(0x00740004)};
      auto const temp = ao::test::TempFile{buildAlacUncompressedFileBytes(options), ".m4a"};
      auto const concreteRes = AlacDecoderSession::open(temp.path, std::nullopt);
      REQUIRE_FALSE(concreteRes);
      CHECK(concreteRes.error().code == Error::Code::InitFailed);
      auto const factoryRes = openDecoderSession(temp.path, std::nullopt);
      REQUIRE_FALSE(factoryRes);
      CHECK(factoryRes.error().code == Error::Code::InitFailed);
    }
  }

  TEST_CASE("AlacDecoderSession - layout declarations cannot repair invalid mandatory cookie geometry or precision",
            "[audio][integration][alac][metadata]")
  {
    struct ConfigMutation final
    {
      std::size_t cookieOffset;
      std::vector<std::uint8_t> bytes;
    };
    auto const mutations = std::vector<ConfigMutation>{
      {12, {0, 0, 0, 0}},    // Zero frame length.
      {12, {0, 0, 0x40, 1}}, // Frame length exceeds the supported allocation bound.
      {16, {1}},             // Unsupported compatible version.
      {17, {0}},
      {17, {12}},
      {17, {33}},
      {21, {0}},
      {21, {9}},
      {32, {0, 0, 0, 0}},    // Zero sample rate.
      {32, {0, 5, 0xDC, 1}}, // 384001 Hz.
    };

    for (auto const& mutation : mutations)
    {
      CAPTURE(mutation.cookieOffset, mutation.bytes);
      auto bytes =
        buildAlacUncompressedFileBytes({.channels = 4, .sampleEntrySuffix = makeAlacChannelLayoutAtom(0x00740004)});
      auto const offset = alacCookieOffset(bytes) + mutation.cookieOffset;
      REQUIRE(offset + mutation.bytes.size() <= bytes.size());
      std::ranges::copy(mutation.bytes, bytes.begin() + static_cast<std::ptrdiff_t>(offset));
      auto const temp = ao::test::TempFile{bytes, ".m4a"};
      auto const concreteRes = AlacDecoderSession::open(temp.path, std::nullopt);
      REQUIRE_FALSE(concreteRes);
      CHECK(concreteRes.error().code == Error::Code::InitFailed);
      CHECK(concreteRes.error().message == "Invalid ALAC stream configuration");
      auto const factoryRes = openDecoderSession(temp.path, std::nullopt);
      REQUIRE_FALSE(factoryRes);
      CHECK(factoryRes.error().code == Error::Code::InitFailed);
      CHECK(factoryRes.error().message == "Invalid ALAC stream configuration");
    }
  }

  TEST_CASE("AlacDecoderSession - requested precision loss remains an error even when optional channel metadata is bad",
            "[audio][integration][alac][metadata]")
  {
    auto good = makeAlacChannelLayoutAtom(0x00740004);
    auto bad = good;
    bad[11] = 1;
    auto const tails = std::vector<std::vector<std::uint8_t>>{{}, good, bad, {0, 0, 0}};

    for (auto const& tail : tails)
    {
      auto const bytes =
        buildAlacUncompressedFileBytes({.channels = 4, .precisionBits = 24, .sampleEntrySuffix = tail});
      auto const temp = ao::test::TempFile{bytes, ".m4a"};
      auto const concreteRes = AlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le);
      REQUIRE_FALSE(concreteRes);
      CHECK(concreteRes.error().code == Error::Code::NotSupported);
      CHECK(concreteRes.error().message == "Requested decoder PCM encoding loses source precision");
      auto const factoryRes = openDecoderSession(temp.path, SampleEncoding::Signed16Le);
      REQUIRE_FALSE(factoryRes);
      CHECK(factoryRes.error().code == Error::Code::NotSupported);
      CHECK(factoryRes.error().message == "Requested decoder PCM encoding loses source precision");
    }
  }
} // namespace ao::audio::test
