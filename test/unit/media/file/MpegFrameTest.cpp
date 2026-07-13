// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "lib/media/file/mpeg/Frame.h"
#include "lib/media/file/mpeg/FrameLayout.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ao::media::file::mpeg::test
{
  namespace
  {
    std::array<std::uint8_t, 4> createHeader(VersionId version,
                                             LayerDescription layer,
                                             std::uint8_t bitrateIndex,
                                             std::uint8_t samplingIndex,
                                             ChannelMode mode = ChannelMode::Stereo,
                                             bool padding = false,
                                             bool protectedByCrc = false)
    {
      auto data = std::array<std::uint8_t, 4>{};
      data[0] = 0xFF;
      data[1] = 0xE0; // Sync
      data[1] |= (static_cast<std::uint8_t>(version) << 3);
      data[1] |= (static_cast<std::uint8_t>(layer) << 1);
      data[1] |= protectedByCrc ? 0 : 1;

      data[2] = (bitrateIndex << 4);
      data[2] |= (samplingIndex << 2);
      data[2] |= (padding ? 2 : 0);

      data[3] = (static_cast<std::uint8_t>(mode) << 6);

      return data;
    }
  } // namespace

  TEST_CASE("MPEG Frame - validates headers", "[media][unit][mpeg][frame]")
  {
    SECTION("Valid V1 Layer III header")
    {
      auto const data = createHeader(VersionId::Ver1, LayerDescription::LayerIII, 9, 0); // 128kbps, 44100Hz
      auto const view = FrameView{data.data(), data.size()};
      CHECK(view.isValid());
      CHECK(view.layout().versionId() == VersionId::Ver1);
      CHECK(view.layout().layer() == LayerDescription::LayerIII);
      CHECK(view.bitrate() == 128000);
      CHECK(view.sampleRate() == 44100);
      CHECK(view.channels() == 2);
    }

    SECTION("Invalid sync")
    {
      auto data = std::to_array<std::uint8_t>({0xFE, 0xE0, 0x00, 0x00});
      auto const view = FrameView{data.data(), data.size()};
      CHECK_FALSE(view.isValid());
    }

    SECTION("Reserved version")
    {
      auto const data = createHeader(VersionId::Reserved, LayerDescription::LayerIII, 9, 0);
      auto const view = FrameView{data.data(), data.size()};
      CHECK_FALSE(view.isValid());
    }

    SECTION("Reserved layer")
    {
      auto const data = createHeader(VersionId::Ver1, LayerDescription::Reserved, 9, 0);
      auto const view = FrameView{data.data(), data.size()};
      CHECK_FALSE(view.isValid());
    }

    SECTION("Free-format bitrate requires adjacent-frame resolution")
    {
      auto const data = createHeader(VersionId::Ver1, LayerDescription::LayerIII, 0, 0);
      auto const view = FrameView{data.data(), data.size()};
      CHECK(view.isValid());
      CHECK(view.length() == 0);
      CHECK(view.bitrate() == 0);
    }

    SECTION("Reserved sampling rate")
    {
      auto const data = createHeader(VersionId::Ver1, LayerDescription::LayerIII, 9, 3);
      auto const view = FrameView{data.data(), data.size()};
      CHECK_FALSE(view.isValid());
    }
  }

  TEST_CASE("MPEG Frame - derives frame properties", "[media][unit][mpeg][frame]")
  {
    SECTION("Layer I")
    {
      auto const data = createHeader(VersionId::Ver1, LayerDescription::LayerI, 9, 0); // 288kbps, 44100Hz
      auto const view = FrameView{data.data(), data.size()};
      CHECK(view.isValid());
      CHECK(view.samplesPerFrame() == 384);
      // Layer I uses 4-byte slots: floor(12 * 288000 / 44100) * 4 = 312.
      CHECK(view.length() == 312);

      auto const paddedData = createHeader(VersionId::Ver1, LayerDescription::LayerI, 9, 0, ChannelMode::Stereo, true);
      CHECK(FrameView{paddedData.data(), paddedData.size()}.length() == 316);
    }

    SECTION("Layer II")
    {
      auto const data = createHeader(VersionId::Ver1, LayerDescription::LayerII, 9, 0); // 160kbps, 44100Hz
      auto const view = FrameView{data.data(), data.size()};
      CHECK(view.isValid());
      CHECK(view.samplesPerFrame() == 1152);
      // Length for Layer II: (1152 / 8 * bitrate * 1000) / samplingRate = (144 * 160 * 1000) / 44100 = 522.44 -> 522
      CHECK(view.length() == 522);
    }

    SECTION("Layer III V2")
    {
      auto const data = createHeader(VersionId::Ver2, LayerDescription::LayerIII, 9, 0); // 80kbps, 22050Hz
      auto const view = FrameView{data.data(), data.size()};
      CHECK(view.isValid());
      CHECK(view.samplesPerFrame() == 576);
      // Length for Layer III V2: (576 / 8 * 80 * 1000) / 22050 = (72 * 80 * 1000) / 22050 = 261.22 -> 261
      CHECK(view.length() == 261);
    }

    SECTION("Single channel")
    {
      auto const data = createHeader(VersionId::Ver1, LayerDescription::LayerIII, 9, 0, ChannelMode::SingleChannel);
      auto const view = FrameView{data.data(), data.size()};
      CHECK(view.channels() == 1);
    }
  }

  TEST_CASE("MPEG Frame - parses Xing information", "[media][unit][mpeg][frame]")
  {
    // Construct a buffer with a frame header followed by Xing header
    auto buffer = std::vector<std::uint8_t>(200, 0);
    auto const header = createHeader(VersionId::Ver1, LayerDescription::LayerIII, 9, 0); // Stereo
    std::ranges::copy(header, buffer.begin());

    // Xing offset for V1 Stereo is 32 + 4 = 36
    auto* xing = reinterpret_cast<XingLayout*>(buffer.data() + 36);
    std::memcpy(xing->magic.data(), "Xing", 4);
    xing->flags = (XingLayout::kFlagFrames | XingLayout::kFlagBytes);

    // Frames field (4 bytes)
    auto* frames = reinterpret_cast<boost::endian::big_uint32_buf_t*>(buffer.data() + 36 + 8);
    *frames = 1234;

    // Bytes field (4 bytes)
    auto* bytes = reinterpret_cast<boost::endian::big_uint32_buf_t*>(buffer.data() + 36 + 12);
    *bytes = 56789;

    auto const view = FrameView{buffer.data(), buffer.size()};
    auto const optXing = view.xingInfo();
    REQUIRE(optXing);
    CHECK(optXing->frames == 1234);
    CHECK(optXing->bytes == 56789);
  }

  TEST_CASE("MPEG Frame - keeps Xing fields inside the current frame", "[media][unit][mpeg][frame]")
  {
    auto const header =
      createHeader(VersionId::Ver2, LayerDescription::LayerIII, 1, 1, ChannelMode::SingleChannel); // 8kbps, 24000Hz
    auto const headerView = FrameView{header.data(), header.size()};
    auto const frameLength = headerView.length();
    auto buffer = std::vector<std::uint8_t>(frameLength + sizeof(boost::endian::big_uint32_buf_t), 0);
    std::ranges::copy(header, buffer.begin());

    static constexpr std::size_t kXingOffsetVer2Mono = 13;
    auto* xing = reinterpret_cast<XingLayout*>(buffer.data() + kXingOffsetVer2Mono);
    std::memcpy(xing->magic.data(), "Xing", 4);
    xing->flags = XingLayout::kFlagFrames;

    auto const framesOffset = kXingOffsetVer2Mono + sizeof(XingLayout);
    REQUIRE(framesOffset < frameLength);
    REQUIRE(framesOffset + sizeof(boost::endian::big_uint32_buf_t) > frameLength);
    auto* frames = reinterpret_cast<boost::endian::big_uint32_buf_t*>(buffer.data() + framesOffset);
    *frames = 98765;

    auto const view = FrameView{buffer.data(), buffer.size()};
    REQUIRE(view.isValid());
    auto const optXing = view.xingInfo();
    REQUIRE(optXing);
    CHECK(optXing->frames == 0);
  }

  TEST_CASE("MPEG Frame - parses Xing information after a protected-frame CRC", "[media][regression][mpeg]")
  {
    auto buffer = std::vector<std::uint8_t>(417, 0);
    auto const header =
      createHeader(VersionId::Ver1, LayerDescription::LayerIII, 9, 0, ChannelMode::Stereo, false, true);
    std::ranges::copy(header, buffer.begin());

    constexpr std::size_t kXingOffset = 4 + 2 + 32;
    auto* xing = reinterpret_cast<XingLayout*>(buffer.data() + kXingOffset);
    std::memcpy(xing->magic.data(), "Xing", 4);
    xing->flags = XingLayout::kFlagFrames;

    auto* frames = reinterpret_cast<boost::endian::big_uint32_buf_t*>(buffer.data() + kXingOffset + sizeof(XingLayout));
    *frames = 4321;

    auto const optXing = FrameView{buffer.data(), buffer.size()}.xingInfo();

    REQUIRE(optXing);
    CHECK(optXing->frames == 4321);
  }

  TEST_CASE("MPEG Frame - locates frame sync", "[media][unit][mpeg][frame]")
  {
    auto const header = createHeader(VersionId::Ver1, LayerDescription::LayerIII, 9, 0, ChannelMode::JointStereo);
    auto const frameLength = FrameView{header.data(), header.size()}.length();
    auto buffer = std::vector<std::uint8_t>{0x00, 0x11, 0x22};
    buffer.insert(buffer.end(), header.begin(), header.end());
    buffer.resize(3 + frameLength);

    auto const optView = locate(buffer.data(), buffer.size());
    REQUIRE(optView);
    CHECK(optView->isValid());
  }

  TEST_CASE("MPEG Frame - locates free-format frames from adjacent sync distance", "[media][regression][mpeg]")
  {
    constexpr std::size_t kFrameLength = 720;
    auto const header = createHeader(VersionId::Ver1, LayerDescription::LayerIII, 0, 1);
    auto buffer = std::vector<std::uint8_t>(kFrameLength * 3, 0);
    std::ranges::copy(header, buffer.begin());
    std::ranges::copy(header, buffer.begin() + static_cast<std::ptrdiff_t>(kFrameLength));
    std::ranges::copy(header, buffer.begin() + static_cast<std::ptrdiff_t>(kFrameLength * 2));

    auto const optView = locate(buffer.data(), buffer.size());

    REQUIRE(optView);
    CHECK(optView->length() == kFrameLength);
    CHECK(optView->bitrate() == 240000);
  }

  TEST_CASE("MPEG Frame - skips an unconfirmed sync candidate in leading junk", "[media][regression][mpeg]")
  {
    auto const header = createHeader(VersionId::Ver1, LayerDescription::LayerIII, 9, 0);
    auto const frameLength = FrameView{header.data(), header.size()}.length();
    constexpr std::size_t kActualFrameOffset = 500;
    auto buffer = std::vector<std::uint8_t>(kActualFrameOffset + (frameLength * 2), 0);
    std::ranges::copy(header, buffer.begin());
    std::ranges::copy(header, buffer.begin() + static_cast<std::ptrdiff_t>(kActualFrameOffset));
    std::ranges::copy(header, buffer.begin() + static_cast<std::ptrdiff_t>(kActualFrameOffset + frameLength));

    auto const optView = locate(buffer.data(), buffer.size());

    REQUIRE(optView);
    CHECK(optView->data() == buffer.data() + kActualFrameOffset);
  }

  TEST_CASE("MPEG Frame - rejects incomplete frame after valid header", "[media][regression][mpeg][frame]")
  {
    auto const header = createHeader(VersionId::Ver1, LayerDescription::LayerIII, 9, 0, ChannelMode::JointStereo);

    auto const optView = locate(header.data(), header.size());

    CHECK_FALSE(optView);
  }

  TEST_CASE("MPEG Frame - rejects a candidate followed only by a matching truncated header",
            "[media][regression][mpeg]")
  {
    auto const header = createHeader(VersionId::Ver1, LayerDescription::LayerIII, 9, 0);
    auto const frameLength = FrameView{header.data(), header.size()}.length();
    auto buffer = std::vector<std::uint8_t>(frameLength + header.size(), 0);
    std::ranges::copy(header, buffer.begin());
    std::ranges::copy(header, buffer.begin() + static_cast<std::ptrdiff_t>(frameLength));

    CHECK_FALSE(locate(buffer.data(), buffer.size()));
  }
} // namespace ao::media::file::mpeg::test
