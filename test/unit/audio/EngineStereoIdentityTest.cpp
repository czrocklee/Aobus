// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EngineTestSupport.h"
#include "FakeCapturingBackend.h"
#include "FlacFrameTestSupport.h"
#include "lib/audio/NullBackend.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/media/mp4/TestAtoms.h"
#include "test/unit/media/wav/TestWav.h"
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/Engine.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/Transport.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    class AlacPacketBitWriter final
    {
    public:
      void write(std::uint32_t value, std::uint8_t bitCount)
      {
        for (auto bit = bitCount; bit > 0; --bit)
        {
          if ((_bitOffset % 8U) == 0U)
          {
            _bytes.push_back(0);
          }

          auto const shift = 7U - (_bitOffset % 8U);
          _bytes.back() |= static_cast<std::uint8_t>(((value >> (bit - 1U)) & 1U) << shift);
          ++_bitOffset;
        }
      }

      std::vector<std::uint8_t> const& bytes() const noexcept { return _bytes; }

    private:
      std::vector<std::uint8_t> _bytes;
      std::size_t _bitOffset = 0;
    };

    std::vector<std::uint8_t> stereoAlacBytes(bool hasUnknownLayout = false)
    {
      // An escaped partial CPE carries sixteen literal stereo pairs.
      auto writer = AlacPacketBitWriter{};
      writer.write(1, 3);
      writer.write(0, 4);
      writer.write(0, 12);
      writer.write(0b1001, 4);
      writer.write(16, 32);

      for (std::uint32_t frame = 0; frame < 16; ++frame)
      {
        writer.write(0x5152, 16);
        writer.write(0x6364, 16);
      }

      writer.write(7, 3);
      auto const& packet = writer.bytes();
      REQUIRE(packet.size() == 72U);
      auto extensions =
        std::vector<std::uint8_t>{0,  0,  0,  36, 'a', 'l', 'a', 'c', 0, 0,  0, 0, 0, 0, 0, 16, 0,    16,
                                  40, 10, 14, 2,  0,   255, 0,   0,   0, 72, 0, 0, 0, 0, 0, 0,  0xBB, 0x80};

      if (hasUnknownLayout)
      {
        // Bounded chan with UseChannelDescriptions and no recognized ALAC tag.
        ao::test::mp4::addAtom(extensions, "chan", std::vector<std::uint8_t>(16, 0));
      }

      auto bytes = std::vector<std::uint8_t>{};
      ao::test::mp4::addAtom(bytes, "mdat", packet);
      auto const track = ao::test::mp4::makeCompleteAudioTrackAtom(
        "alac", extensions, 48000, 16, static_cast<std::uint32_t>(packet.size()), 16, 8);
      ao::test::mp4::addAtom(bytes, "moov", track);
      return bytes;
    }

    std::vector<std::uint8_t> stereoAlacPcm()
    {
      auto bytes = std::vector<std::uint8_t>{};
      bytes.reserve(64);

      for (std::uint32_t frame = 0; frame < 16; ++frame)
      {
        // Independent little-endian oracle, not packet-writer output.
        bytes.insert(bytes.end(), {0x52, 0x51, 0x64, 0x63});
      }

      return bytes;
    }

    std::vector<std::uint8_t> stereoFlacBytes()
    {
      auto bytes = buildFlacStreaminfoBytes(16);
      auto const markers = std::to_array<std::int32_t>({0x1211, 0x2423});
      auto const frame = buildFlacVerbatimFrameBytes(0, 48000, 2, 16, markers);
      bytes.insert(bytes.end(), frame.begin(), frame.end());
      return bytes;
    }

    std::vector<std::uint8_t> stereoFlacPcm()
    {
      auto bytes = std::vector<std::uint8_t>{};
      bytes.reserve(64);

      for (std::uint32_t frame = 0; frame < 16; ++frame)
      {
        bytes.insert(bytes.end(), {0x11, 0x12, 0x23, 0x24});
      }

      return bytes;
    }
  } // namespace

  TEST_CASE("Engine - real WAV FLAC and ALAC splice with exact stereo identity",
            "[audio][integration][engine][gapless][concurrency]")
  {
    auto const [left, right] = GENERATE(std::pair<std::size_t, std::size_t>{0, 1});
    bool const reverse = GENERATE(false, true);
    CAPTURE(left, right, reverse);
    auto const alac = ao::test::TempFile{stereoAlacBytes(), ".m4a"};
    auto const flac = ao::test::TempFile{stereoFlacBytes(), ".flac"};
    auto const wavPcm = std::vector<std::uint8_t>{0x35, 0x36, 0x47, 0x48, 0x59, 0x5A, 0x6B, 0x6C};
    auto const wav =
      ao::test::TempFile{ao::test::wav::makeWav({.channels = 2, .sampleRate = 48000, .audioData = wavPcm}), ".wav"};
    auto const flacPcm = stereoFlacPcm();
    auto const alacPcm = stereoAlacPcm();
    auto const paths = std::array{std::cref(wav.path), std::cref(flac.path), std::cref(alac.path)};
    auto const contents = std::array{std::cref(wavPcm), std::cref(flacPcm), std::cref(alacPcm)};
    auto const first = reverse ? right : left;
    auto const second = reverse ? left : right;
    auto const& firstPath = paths[first].get();
    auto const& secondPath = paths[second].get();
    auto const& firstPcm = contents[first].get();
    auto const& secondPcm = contents[second].get();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    auto advanced = CallbackLatch{};
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice()};
    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advanced.notify(); });
    engine.play(makePlaybackItem(firstPath));
    REQUIRE(engine.status().transport == Transport::Playing);
    auto const openedFormat = backend->currentFormat();
    CHECK(openedFormat.speakerMask == 0x3U);
    CHECK(engine.status().routeState.sourceFormat.speakerMask == 0x3U);
    CHECK(engine.status().routeState.sourceChannelLayout.origin == ChannelLayoutOrigin::Identified);
    auto const preparedRes = engine.setNext(makePlaybackItem(secondPath));
    REQUIRE(preparedRes);
    REQUIRE(preparedRes->transition == Engine::PreparedTransitionMode::Gapless);
    auto* const target = backend->target();
    REQUIRE(target != nullptr);
    auto const secondPrefix = secondPcm.size() / 2U;
    auto expected = firstPcm;
    auto const secondHead = std::span<std::uint8_t const>{secondPcm}.first(secondPrefix);
    expected.insert(expected.end(), secondHead.begin(), secondHead.end());
    auto output = std::vector<std::byte>(expected.size());
    auto const rendered = target->renderPcm(output);
    REQUIRE(rendered.bytesWritten == output.size());
    CHECK_FALSE(rendered.drained);
    CHECK(std::ranges::equal(output, utility::bytes::view(std::span<std::uint8_t const>{expected})));
    CHECK(rendered.positionFrameOffset == firstPcm.size() / 4U);
    CHECK(rendered.positionFrames == secondPrefix / 4U);
    target->handlePositionAdvanced(rendered.positionFrames);
    REQUIRE(advanced.tryWaitForCount(1));
    CHECK(engine.status().routeState.sourceFormat.speakerMask == 0x3U);
    CHECK(engine.status().routeState.decoderOutputFormat.speakerMask == 0x3U);
    CHECK(engine.status().routeState.engineOutputFormat.speakerMask == 0x3U);
    CHECK(engine.status().routeState.sourceChannelLayout.origin == ChannelLayoutOrigin::Identified);

    // Reverse promotion retains the same installed mode and literal tail.
    auto const thirdRes = engine.setNext(makePlaybackItem(firstPath));
    REQUIRE(thirdRes);
    REQUIRE(thirdRes->transition == Engine::PreparedTransitionMode::Gapless);
    auto const secondTail = std::span<std::uint8_t const>{secondPcm}.subspan(secondPrefix);
    auto thirdExpected = std::vector<std::uint8_t>{secondTail.begin(), secondTail.end()};
    thirdExpected.insert(thirdExpected.end(), firstPcm.begin(), firstPcm.end());
    auto thirdOutput = std::vector<std::byte>(thirdExpected.size());
    auto const thirdRendered = target->renderPcm(thirdOutput);
    REQUIRE(thirdRendered.bytesWritten == thirdOutput.size());
    CHECK_FALSE(thirdRendered.drained);
    CHECK(std::ranges::equal(thirdOutput, utility::bytes::view(std::span<std::uint8_t const>{thirdExpected})));
    target->handlePositionAdvanced(thirdRendered.positionFrames);
    REQUIRE(advanced.tryWaitForCount(2));
    CHECK(engine.status().routeState.sourceFormat.speakerMask == 0x3U);
    CHECK(backend->currentFormat() == openedFormat);
    CHECK(std::ranges::count(backend->events(), std::string{"open"}, &FakeCapturingBackend::Event::name) == 1);
    CHECK(std::ranges::count(backend->events(), std::string{"start"}, &FakeCapturingBackend::Event::name) == 1);
    auto finalOutput = std::array<std::byte, 4>{};
    auto const drained = target->renderPcm(finalOutput);
    CHECK(drained.bytesWritten == 0U);
    CHECK(drained.drained);
  }

  TEST_CASE("Engine - unidentified ALAC stereo drains before reopening an identified successor",
            "[audio][integration][engine][gapless][concurrency]")
  {
    bool const wavFirst = GENERATE(false, true);
    CAPTURE(wavFirst);
    auto const alac = ao::test::TempFile{stereoAlacBytes(true), ".m4a"};
    auto const wavPcm = std::vector<std::uint8_t>{0x35, 0x36, 0x47, 0x48};
    auto const wav =
      ao::test::TempFile{ao::test::wav::makeWav({.channels = 2, .sampleRate = 48000, .audioData = wavPcm}), ".wav"};
    auto const alacPcm = stereoAlacPcm();
    auto const& firstPath = wavFirst ? wav.path : alac.path;
    auto const& secondPath = wavFirst ? alac.path : wav.path;
    auto const& firstPcm = wavFirst ? wavPcm : alacPcm;
    auto const& secondPcm = wavFirst ? alacPcm : wavPcm;
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    auto advanced = CallbackLatch{};
    auto ended = CallbackLatch{};
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice()};
    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advanced.notify(); });
    engine.setOnTrackEnded([&](Engine::TrackEnded const&) { ended.notify(); });
    engine.play(makePlaybackItem(firstPath));
    REQUIRE(engine.status().transport == Transport::Playing);
    CHECK(backend->currentFormat().speakerMask == (wavFirst ? 0x3U : 0U));
    auto const preparedRes = engine.setNext(makePlaybackItem(secondPath));
    REQUIRE(preparedRes);
    REQUIRE(preparedRes->transition == Engine::PreparedTransitionMode::DrainFallback);
    auto* const target = backend->target();
    REQUIRE(target != nullptr);
    auto output = std::vector<std::byte>(firstPcm.size() + 4U);
    auto const rendered = target->renderPcm(output);
    REQUIRE(rendered.bytesWritten == firstPcm.size());
    CHECK_FALSE(rendered.drained);
    CHECK(std::ranges::equal(
      std::span{output}.first(firstPcm.size()), utility::bytes::view(std::span<std::uint8_t const>{firstPcm})));
    auto finalOutput = std::array<std::byte, 4>{};
    auto const drained = target->renderPcm(finalOutput);
    REQUIRE(drained.bytesWritten == 0U);
    REQUIRE(drained.drained);
    CHECK(advanced.count() == 0U);
    backend->emitDrainComplete();
    REQUIRE(ended.tryWaitForCount(1));
    REQUIRE(engine.status().transport == Transport::Idle);
    engine.play(makePlaybackItem(secondPath));
    REQUIRE(engine.status().transport == Transport::Playing);
    CHECK(backend->currentFormat().speakerMask == (wavFirst ? 0U : 0x3U));
    CHECK(std::ranges::count(backend->events(), std::string{"open"}, &FakeCapturingBackend::Event::name) == 2);
    auto* const secondTarget = backend->target();
    REQUIRE(secondTarget != nullptr);
    auto secondOutput = std::vector<std::byte>(secondPcm.size());
    auto const secondRendered = secondTarget->renderPcm(secondOutput);
    REQUIRE(secondRendered.bytesWritten == secondPcm.size());
    CHECK(std::ranges::equal(secondOutput, utility::bytes::view(std::span<std::uint8_t const>{secondPcm})));
  }

  TEST_CASE("Engine - different PCM identities require a drained transition", "[audio][integration][engine][gapless]")
  {
    auto const first = ao::test::TempFile{
      ao::test::wav::makeWav({.channels = 2, .sampleRate = 48000, .audioData = {0x11, 0x12, 0x23, 0x24}}), ".wav"};
    auto nextSpec = ao::test::wav::Spec{.sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
                                        .channels = 2,
                                        .sampleRate = 48000,
                                        .channelMask = 0x30,
                                        .audioData = {0x35, 0x36, 0x47, 0x48}};

    SECTION("known alternate roles")
    {
    }

    SECTION("different rate")
    {
      nextSpec.sampleRate = 44100;
      nextSpec.channelMask = 0x3;
    }

    SECTION("different count")
    {
      nextSpec.channels = 1;
      nextSpec.channelMask = 0x4;
      nextSpec.audioData = {0x35, 0x36};
    }

    auto const next = ao::test::TempFile{ao::test::wav::makeWav(nextSpec), ".wav"};
    auto engine = Engine{std::make_unique<NullBackend>(), makeEngineTestDevice()};
    engine.play(makePlaybackItem(first.path));
    REQUIRE(engine.status().transport == Transport::Playing);
    auto const preparedRes = engine.setNext(makePlaybackItem(next.path));
    REQUIRE(preparedRes);
    CHECK(preparedRes->transition == Engine::PreparedTransitionMode::DrainFallback);
  }

  TEST_CASE("Engine - successor precision remains bounded by the confirmed endpoint behind a wider client",
            "[audio][integration][engine][gapless][concurrency]")
  {
    auto const first =
      ao::test::TempFile{ao::test::wav::makeWav({.channels = 2, .audioData = {0x11, 0x12, 0x23, 0x24}}), ".wav"};
    auto const nextBits = GENERATE(std::uint16_t{16}, std::uint16_t{24});
    auto const next = ao::test::TempFile{
      ao::test::wav::makeWav({.channels = 2,
                              .bitsPerSample = nextBits,
                              .audioData = nextBits == 16U
                                             ? std::vector<std::uint8_t>{0x35, 0x36, 0x47, 0x48, 0x59, 0x5A, 0x6B, 0x6C}
                                             : std::vector<std::uint8_t>{0x35, 0x36, 0x37, 0x47, 0x48, 0x49}}),
      ".wav"};
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    backendPtr->setSelectedEncoding(SampleEncoding::Signed32Le);
    backendPtr->setConfirmedEndpointPrecision(16);
    auto* const backend = backendPtr.get();
    auto advanced = CallbackLatch{};
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice()};
    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advanced.notify(); });
    engine.play(makePlaybackItem(first.path));
    REQUIRE(engine.status().transport == Transport::Playing);
    auto const preparedRes = engine.setNext(makePlaybackItem(next.path));
    REQUIRE(preparedRes);
    CHECK(preparedRes->transition ==
          (nextBits == 16U ? Engine::PreparedTransitionMode::Gapless : Engine::PreparedTransitionMode::DrainFallback));

    if (nextBits == 16U)
    {
      auto* const target = backend->target();
      REQUIRE(target != nullptr);
      auto output = std::array<std::byte, 16>{};
      auto const rendered = target->renderPcm(output);
      REQUIRE(rendered.bytesWritten == output.size());
      REQUIRE_FALSE(rendered.drained);
      target->handlePositionAdvanced(rendered.positionFrames);
      REQUIRE(advanced.tryWaitForCount(1));
      REQUIRE(engine.status().transport == Transport::Playing);
      auto const expected = std::array<std::uint8_t, 16>{
        0x00, 0x00, 0x11, 0x12, 0x00, 0x00, 0x23, 0x24, 0x00, 0x00, 0x35, 0x36, 0x00, 0x00, 0x47, 0x48};
      CHECK(std::ranges::equal(output, utility::bytes::view(expected)));
      CHECK(std::ranges::count(backend->events(), std::string{"open"}, &FakeCapturingBackend::Event::name) == 1);
      auto const third = ao::test::TempFile{
        ao::test::wav::makeWav({.channels = 2, .bitsPerSample = 24, .audioData = {0x51, 0x52, 0x53, 0x61, 0x62, 0x63}}),
        ".wav"};
      auto const thirdRes = engine.setNext(makePlaybackItem(third.path));
      REQUIRE(thirdRes);
      // Promotion changes the source, never the endpoint bound for this open.
      CHECK(thirdRes->transition == Engine::PreparedTransitionMode::DrainFallback);
    }
  }
} // namespace ao::audio::test
