// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EngineTestSupport.h"
#include "FakeCapturingBackend.h"
#include "FlacFrameTestSupport.h"
#include "lib/audio/AlacDecoderSession.h"
#include "lib/audio/NullBackend.h"
#include "lib/audio/detail/TrackPreparation.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/media/mp4/TestAtoms.h"
#include "test/unit/media/wav/TestWav.h"
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/Engine.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/Transport.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    class PermissiveModeBackend final : public NullBackend
    {
    public:
      bool canReusePcmMode(PcmFormat const& /*current*/, PcmFormat const& /*next*/) const noexcept override
      {
        return true;
      }
    };

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

    std::vector<std::uint8_t> stereoAlacBytes()
    {
      // Apple ALACDecoder.cpp's escaped CPE grammar: no predictor or encoder
      // dependency. One partial frame contains sixteen literal stereo pairs.
      auto writer = AlacPacketBitWriter{};
      writer.write(1, 3);      // ID_CPE
      writer.write(0, 4);      // Element instance
      writer.write(0, 12);     // Unused header
      writer.write(0b1001, 4); // Partial frame, no shifted bytes, escaped samples
      writer.write(16, 32);

      for (std::uint32_t frame = 0; frame < 16; ++frame)
      {
        writer.write(0x5152, 16);
        writer.write(0x6364, 16);
      }

      writer.write(7, 3); // ID_END; unwritten low bits remain zero padding.
      auto const& packet = writer.bytes();
      REQUIRE(packet.size() == 72U);

      // 36-byte alac atom: version/flags, then ALACSpecificConfig in big endian.
      // Frame length 16, version 0, depth 16, pb/mb/kb 40/10/14, stereo,
      // maxRun 255, maxFrameBytes 72, unknown average bitrate, sample rate 48000.
      auto const cookie =
        std::vector<std::uint8_t>{0,  0,  0,  36, 'a', 'l', 'a', 'c', 0, 0,  0, 0, 0, 0, 0, 16, 0,    16,
                                  40, 10, 14, 2,  0,   255, 0,   0,   0, 72, 0, 0, 0, 0, 0, 0,  0xBB, 0x80};
      auto bytes = std::vector<std::uint8_t>{};
      ao::test::mp4::addAtom(bytes, "mdat", packet);
      auto const track = ao::test::mp4::makeCompleteAudioTrackAtom(
        "alac", cookie, 48000, 16, static_cast<std::uint32_t>(packet.size()), 16, 8);
      ao::test::mp4::addAtom(bytes, "moov", track);
      return bytes;
    }

    std::vector<std::uint8_t> stereoAlacPcm()
    {
      auto bytes = std::vector<std::uint8_t>{};
      bytes.reserve(64);

      // Independent literal little-endian oracle, not packet-writer output or
      // decoder interleaving. Distinct from the FLAC fixture's channel markers.
      for (std::uint32_t frame = 0; frame < 16; ++frame)
      {
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

      // Literal little-endian channel markers, independent of the decoder's
      // interleaving and the FLAC fixture's CRC/verbatim encoding machinery.
      for (std::uint32_t frame = 0; frame < 16; ++frame)
      {
        bytes.insert(bytes.end(), {0x11, 0x12, 0x23, 0x24});
      }

      return bytes;
    }
  } // namespace

  TEST_CASE("Engine - real WAV and FLAC reuse exact stereo identity without a cross-mask proof",
            "[audio][integration][engine][gapless][concurrency]")
  {
    bool const wavFirst = GENERATE(false, true);
    CAPTURE(wavFirst);
    auto const flac = ao::test::TempFile{stereoFlacBytes(), ".flac"};
    auto const wavPcm = std::vector<std::uint8_t>{0x35, 0x36, 0x47, 0x48, 0x59, 0x5A, 0x6B, 0x6C};
    auto const wavBytes = ao::test::wav::makeWav({.channels = 2, .sampleRate = 48000, .audioData = wavPcm});
    auto const wav = ao::test::TempFile{wavBytes, ".wav"};
    auto const flacPcm = stereoFlacPcm();
    auto const& firstPcm = wavFirst ? wavPcm : flacPcm;
    auto const& secondPcm = wavFirst ? flacPcm : wavPcm;
    auto const& firstPath = wavFirst ? wav.path : flac.path;
    auto const& secondPath = wavFirst ? flac.path : wav.path;
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    backend->setStereoModeReuseAllowed(false);
    auto advanced = CallbackLatch{};
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice()};
    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advanced.notify(); });
    engine.play(makePlaybackItem(firstPath));
    REQUIRE(engine.status().transport == Transport::Playing);
    auto const openedFormat = backend->currentFormat();
    CHECK(openedFormat.speakerMask == 0x3U);
    CHECK(engine.status().routeState.sourceFormat.speakerMask == 0x3U);
    CHECK(engine.status().routeState.sourceChannelLayout.origin == ChannelLayoutOrigin::FormatDefault);
    auto const preparedRes = engine.setNext(makePlaybackItem(secondPath));
    REQUIRE(preparedRes);
    REQUIRE(preparedRes->transition == Engine::PreparedTransitionMode::Gapless);
    auto* const target = backend->target();
    REQUIRE(target != nullptr);
    auto const secondPrefix = secondPcm.size() / 2U;
    auto const secondHead = std::span<std::uint8_t const>{secondPcm}.first(secondPrefix);
    auto const secondTail = std::span<std::uint8_t const>{secondPcm}.subspan(secondPrefix);
    auto expected = firstPcm;
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
    auto const status = engine.status();
    CHECK(status.routeState.sourceFormat.speakerMask == 0x3U);
    CHECK(status.routeState.decoderOutputFormat.speakerMask == 0x3U);
    CHECK(status.routeState.engineOutputFormat.speakerMask == 0x3U);
    CHECK(status.routeState.sourceChannelLayout.origin == ChannelLayoutOrigin::FormatDefault);
    CHECK(backend->currentFormat() == openedFormat);
    CHECK(std::ranges::count(backend->events(), std::string{"open"}, &FakeCapturingBackend::Event::name) == 1);
    CHECK(std::ranges::count(backend->events(), std::string{"start"}, &FakeCapturingBackend::Event::name) == 1);

    // Promote in reverse without reopening or inventing native mapping proof.
    auto const thirdRes = engine.setNext(makePlaybackItem(firstPath));
    REQUIRE(thirdRes);
    REQUIRE(thirdRes->transition == Engine::PreparedTransitionMode::Gapless);
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
    CHECK(engine.status().routeState.decoderOutputFormat.speakerMask == 0x3U);
    CHECK(engine.status().routeState.engineOutputFormat.speakerMask == 0x3U);
    CHECK(engine.status().routeState.sourceChannelLayout.origin == ChannelLayoutOrigin::FormatDefault);
    CHECK(backend->currentFormat() == openedFormat);
    CHECK(std::ranges::count(backend->events(), std::string{"open"}, &FakeCapturingBackend::Event::name) == 1);
    CHECK(std::ranges::count(backend->events(), std::string{"start"}, &FakeCapturingBackend::Event::name) == 1);
    auto finalOutput = std::array<std::byte, 4>{};
    auto const drained = target->renderPcm(finalOutput);
    CHECK(drained.bytesWritten == 0U);
    CHECK(drained.drained);
  }

  TEST_CASE("AlacDecoderSession - escaped stereo fixture preserves literal PCM with unknown roles",
            "[audio][integration][alac]")
  {
    auto const alac = ao::test::TempFile{stereoAlacBytes(), ".m4a"};
    auto decoderRes = AlacDecoderSession::open(alac.path, SampleEncoding::Signed16Le);
    REQUIRE(decoderRes);
    auto const info = (*decoderRes)->streamInfo();
    CHECK(info.sourceFormat.channels == 2U);
    CHECK(info.sourceFormat.sampleRate == 48000U);
    CHECK(info.sourceFormat.precisionBits == 16U);
    CHECK(info.sourceFormat.speakerMask == 0U);
    CHECK(info.outputFormat.speakerMask == 0U);
    CHECK(info.sourceChannelLayout.origin == ChannelLayoutOrigin::Unknown);
    auto const blockRes = (*decoderRes)->readNextBlock();
    REQUIRE(blockRes);
    REQUIRE(blockRes->frames == 16U);
    CHECK(blockRes->firstFrameIndex == 0U);
    CHECK(blockRes->endOfStream);
    auto const expected = stereoAlacPcm();
    CHECK(std::ranges::equal(blockRes->bytes, utility::bytes::view(std::span<std::uint8_t const>{expected})));
    auto const endRes = (*decoderRes)->readNextBlock();
    REQUIRE(endRes);
    CHECK(endRes->bytes.empty());
    CHECK(endRes->frames == 0U);
    CHECK(endRes->endOfStream);
  }

  TEST_CASE("Engine - real WAV and ALAC splice only with a backend-proved stereo transport",
            "[audio][integration][engine][gapless][concurrency]")
  {
    bool const wavFirst = GENERATE(false, true);
    bool const permitsReuse = GENERATE(false, true);
    CAPTURE(wavFirst, permitsReuse);
    auto const alac = ao::test::TempFile{stereoAlacBytes(), ".m4a"};
    auto const wavPcm = std::vector<std::uint8_t>{0x35, 0x36, 0x47, 0x48, 0x59, 0x5A, 0x6B, 0x6C};
    auto const wavBytes = ao::test::wav::makeWav({.channels = 2, .sampleRate = 48000, .audioData = wavPcm});
    auto const wav = ao::test::TempFile{wavBytes, ".wav"};
    auto const alacPcm = stereoAlacPcm();
    auto const& firstPcm = wavFirst ? wavPcm : alacPcm;
    auto const& secondPcm = wavFirst ? alacPcm : wavPcm;
    auto const& firstPath = wavFirst ? wav.path : alac.path;
    auto const& secondPath = wavFirst ? alac.path : wav.path;
    auto const initialMask = wavFirst ? 0x3U : 0U;
    auto const nextMask = wavFirst ? 0U : 0x3U;
    auto const initialOrigin = wavFirst ? ChannelLayoutOrigin::FormatDefault : ChannelLayoutOrigin::Unknown;
    auto const nextOrigin = wavFirst ? ChannelLayoutOrigin::Unknown : ChannelLayoutOrigin::FormatDefault;
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    // This fake models an established transport. Separate native-plan tests
    // prove each real backend; the fake does not establish native compatibility.
    backend->setStereoModeReuseAllowed(permitsReuse);
    auto advanced = CallbackLatch{};
    auto ended = CallbackLatch{};
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice()};
    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advanced.notify(); });
    engine.setOnTrackEnded([&](Engine::TrackEnded const&) { ended.notify(); });
    engine.play(makePlaybackItem(firstPath));
    REQUIRE(engine.status().transport == Transport::Playing);
    auto const openedFormat = backend->currentFormat();
    CHECK(openedFormat.speakerMask == initialMask);
    CHECK(engine.status().routeState.sourceFormat.speakerMask == initialMask);
    CHECK(engine.status().routeState.sourceChannelLayout.origin == initialOrigin);
    auto const preparedRes = engine.setNext(makePlaybackItem(secondPath));
    REQUIRE(preparedRes);
    REQUIRE(preparedRes->transition ==
            (permitsReuse ? Engine::PreparedTransitionMode::Gapless : Engine::PreparedTransitionMode::DrainFallback));
    auto* const target = backend->target();
    REQUIRE(target != nullptr);
    auto const secondPrefix = secondPcm.size() / 2U;
    auto const secondHead = std::span<std::uint8_t const>{secondPcm}.first(secondPrefix);
    auto const secondTail = std::span<std::uint8_t const>{secondPcm}.subspan(secondPrefix);
    auto output = std::vector<std::byte>(firstPcm.size() + secondPrefix);
    auto const rendered = target->renderPcm(output);
    target->handlePositionAdvanced(rendered.positionFrames);

    if (permitsReuse)
    {
      auto expected = firstPcm;
      expected.insert(expected.end(), secondHead.begin(), secondHead.end());
      REQUIRE(rendered.bytesWritten == output.size());
      REQUIRE_FALSE(rendered.drained);
      CHECK(std::ranges::equal(output, utility::bytes::view(std::span<std::uint8_t const>{expected})));
      CHECK(rendered.positionFrameOffset == firstPcm.size() / 4U);
      CHECK(rendered.positionFrames == secondPrefix / 4U);
      REQUIRE(advanced.tryWaitForCount(1));
      CHECK(ended.count() == 0U);
      auto const status = engine.status();
      CHECK(status.routeState.sourceFormat.speakerMask == nextMask);
      CHECK(status.routeState.decoderOutputFormat.speakerMask == nextMask);
      CHECK(status.routeState.engineOutputFormat.speakerMask == nextMask);
      CHECK(status.routeState.sourceChannelLayout.origin == nextOrigin);
      // Native configuration is not rewritten to fabricate a new observation.
      CHECK(backend->currentFormat() == openedFormat);
      CHECK(std::ranges::count(backend->events(), std::string{"open"}, &FakeCapturingBackend::Event::name) == 1);
      CHECK(std::ranges::count(backend->events(), std::string{"start"}, &FakeCapturingBackend::Event::name) == 1);

      // Reuse again in the opposite direction after transition publication:
      // the installed native mode remains the original one throughout.
      auto const thirdRes = engine.setNext(makePlaybackItem(firstPath));
      REQUIRE(thirdRes);
      REQUIRE(thirdRes->transition == Engine::PreparedTransitionMode::Gapless);
      auto thirdExpected = std::vector<std::uint8_t>{secondTail.begin(), secondTail.end()};
      thirdExpected.insert(thirdExpected.end(), firstPcm.begin(), firstPcm.end());
      auto thirdOutput = std::vector<std::byte>(thirdExpected.size());
      auto const thirdRendered = target->renderPcm(thirdOutput);
      REQUIRE(thirdRendered.bytesWritten == thirdOutput.size());
      CHECK_FALSE(thirdRendered.drained);
      target->handlePositionAdvanced(thirdRendered.positionFrames);
      CHECK(std::ranges::equal(thirdOutput, utility::bytes::view(std::span<std::uint8_t const>{thirdExpected})));
      REQUIRE(advanced.tryWaitForCount(2));
      CHECK(engine.status().routeState.sourceFormat.speakerMask == initialMask);
      CHECK(engine.status().routeState.decoderOutputFormat.speakerMask == initialMask);
      CHECK(engine.status().routeState.engineOutputFormat.speakerMask == initialMask);
      CHECK(engine.status().routeState.sourceChannelLayout.origin == initialOrigin);
      CHECK(backend->currentFormat() == openedFormat);
      CHECK(std::ranges::count(backend->events(), std::string{"open"}, &FakeCapturingBackend::Event::name) == 1);
      CHECK(std::ranges::count(backend->events(), std::string{"start"}, &FakeCapturingBackend::Event::name) == 1);
      auto finalOutput = std::array<std::byte, 4>{};
      auto const drained = target->renderPcm(finalOutput);
      REQUIRE(drained.bytesWritten == 0U);
      REQUIRE(drained.drained);
    }
    else
    {
      REQUIRE(rendered.bytesWritten == firstPcm.size());
      CHECK_FALSE(rendered.drained);
      CHECK(std::ranges::equal(
        std::span{output}.first(firstPcm.size()), utility::bytes::view(std::span<std::uint8_t const>{firstPcm})));
      // A nonempty final prefix is committed first. Only the following empty
      // render closes admission and permits the native drain-complete event.
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
      CHECK(backend->currentFormat().speakerMask == nextMask);
      CHECK(engine.status().routeState.sourceChannelLayout.origin == nextOrigin);
      CHECK(std::ranges::count(backend->events(), std::string{"open"}, &FakeCapturingBackend::Event::name) == 2);
      CHECK(std::ranges::count(backend->events(), std::string{"start"}, &FakeCapturingBackend::Event::name) == 2);

      // Refusal remains effective in reverse; opening the successor manually
      // must deliver its literal samples rather than silently splicing either.
      auto const thirdRes = engine.setNext(makePlaybackItem(firstPath));
      REQUIRE(thirdRes);
      REQUIRE(thirdRes->transition == Engine::PreparedTransitionMode::DrainFallback);
      auto* const secondTarget = backend->target();
      REQUIRE(secondTarget != nullptr);
      auto secondOutput = std::vector<std::byte>(secondPcm.size() + 4U);
      auto const secondRendered = secondTarget->renderPcm(secondOutput);
      REQUIRE(secondRendered.bytesWritten == secondPcm.size());
      CHECK_FALSE(secondRendered.drained);
      CHECK(std::ranges::equal(std::span{secondOutput}.first(secondPcm.size()),
                               utility::bytes::view(std::span<std::uint8_t const>{secondPcm})));
      auto const secondDrained = secondTarget->renderPcm(finalOutput);
      REQUIRE(secondDrained.bytesWritten == 0U);
      REQUIRE(secondDrained.drained);
      CHECK(advanced.count() == 0U);
      backend->emitDrainComplete();
      REQUIRE(ended.tryWaitForCount(2));
      REQUIRE(engine.status().transport == Transport::Idle);
      engine.play(makePlaybackItem(firstPath));
      REQUIRE(engine.status().transport == Transport::Playing);
      CHECK(backend->currentFormat().speakerMask == initialMask);
      CHECK(engine.status().routeState.sourceChannelLayout.origin == initialOrigin);
      CHECK(std::ranges::count(backend->events(), std::string{"open"}, &FakeCapturingBackend::Event::name) == 3);
      CHECK(std::ranges::count(backend->events(), std::string{"start"}, &FakeCapturingBackend::Event::name) == 3);
      auto* const repeatedTarget = backend->target();
      REQUIRE(repeatedTarget != nullptr);
      auto repeatedOutput = std::vector<std::byte>(firstPcm.size() + 4U);
      auto const repeatedRendered = repeatedTarget->renderPcm(repeatedOutput);
      REQUIRE(repeatedRendered.bytesWritten == firstPcm.size());
      CHECK_FALSE(repeatedRendered.drained);
      CHECK(std::ranges::equal(std::span{repeatedOutput}.first(firstPcm.size()),
                               utility::bytes::view(std::span<std::uint8_t const>{firstPcm})));
      auto const repeatedDrained = repeatedTarget->renderPcm(finalOutput);
      REQUIRE(repeatedDrained.bytesWritten == 0U);
      REQUIRE(repeatedDrained.drained);
    }
  }

  TEST_CASE("Engine - real ALAC and WAV stereo transport proof is rechecked after decoder preparation",
            "[audio][integration][engine][gapless][concurrency]")
  {
    bool const wavFirst = GENERATE(false, true);
    CAPTURE(wavFirst);
    auto const alac = ao::test::TempFile{stereoAlacBytes(), ".m4a"};
    auto const wavPcm = std::vector<std::uint8_t>{0x35, 0x36, 0x47, 0x48};
    auto const wavBytes = ao::test::wav::makeWav({.channels = 2, .sampleRate = 48000, .audioData = wavPcm});
    auto const wav = ao::test::TempFile{wavBytes, ".wav"};
    auto const alacPcm = stereoAlacPcm();
    auto const& firstPath = wavFirst ? wav.path : alac.path;
    auto const& secondPath = wavFirst ? alac.path : wav.path;
    auto const& expected = wavFirst ? wavPcm : alacPcm;
    auto const initialMask = wavFirst ? 0x3U : 0U;
    auto const initialOrigin = wavFirst ? ChannelLayoutOrigin::FormatDefault : ChannelLayoutOrigin::Unknown;
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    backend->setStereoModeReuseAllowed(true);
    auto advanced = CallbackLatch{};
    auto ended = CallbackLatch{};
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice()};
    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advanced.notify(); });
    engine.setOnTrackEnded([&](Engine::TrackEnded const&) { ended.notify(); });
    engine.play(makePlaybackItem(firstPath));
    REQUIRE(engine.status().transport == Transport::Playing);
    auto const openedFormat = backend->currentFormat();
    CHECK(openedFormat.speakerMask == initialMask);
    auto preparationRes = detail::TrackPreparation::capture(engine,
                                                            makePlaybackItem(secondPath),
                                                            std::chrono::milliseconds{0},
                                                            detail::TrackPreparation::Purpose::GaplessLookahead);
    REQUIRE(preparationRes);
    REQUIRE(preparationRes->inspect());
    REQUIRE(preparationRes->selectPrewarmFormat(engine));
    REQUIRE(preparationRes->prepare());
    backend->setStereoModeReuseAllowed(false);
    auto const adoptedRes = std::move(*preparationRes).adoptNext(engine);
    REQUIRE(adoptedRes);
    REQUIRE(adoptedRes->transition == Engine::PreparedTransitionMode::DrainFallback);
    auto* const target = backend->target();
    REQUIRE(target != nullptr);
    auto output = std::vector<std::byte>(expected.size() + 4U);
    auto const rendered = target->renderPcm(output);
    REQUIRE(rendered.bytesWritten == expected.size());
    CHECK_FALSE(rendered.drained);
    CHECK(std::ranges::equal(
      std::span{output}.first(expected.size()), utility::bytes::view(std::span<std::uint8_t const>{expected})));
    auto finalOutput = std::array<std::byte, 4>{};
    auto const drained = target->renderPcm(finalOutput);
    REQUIRE(drained.bytesWritten == 0U);
    REQUIRE(drained.drained);
    CHECK(advanced.count() == 0U);
    CHECK(engine.status().routeState.sourceFormat.speakerMask == initialMask);
    CHECK(engine.status().routeState.sourceChannelLayout.origin == initialOrigin);
    CHECK(backend->currentFormat() == openedFormat);
    CHECK(std::ranges::count(backend->events(), std::string{"open"}, &FakeCapturingBackend::Event::name) == 1);
    CHECK(std::ranges::count(backend->events(), std::string{"start"}, &FakeCapturingBackend::Event::name) == 1);
    backend->emitDrainComplete();
    REQUIRE(ended.tryWaitForCount(1));
    CHECK(engine.status().transport == Transport::Idle);
  }

  TEST_CASE("Engine - a backend assertion cannot widen the closed stereo reuse candidate",
            "[audio][integration][engine][gapless]")
  {
    auto const firstBytes =
      ao::test::wav::makeWav({.channels = 2, .sampleRate = 48000, .audioData = {0x11, 0x12, 0x23, 0x24}});
    auto const first = ao::test::TempFile{firstBytes, ".wav"};
    auto nextSpec = ao::test::wav::Spec{.sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
                                        .channels = 2,
                                        .sampleRate = 48000,
                                        .channelMask = 0x30,
                                        .audioData = {0x35, 0x36, 0x47, 0x48}};

    SECTION("known alternate roles remain a different identity")
    {
    }

    SECTION("a new rate remains different storage")
    {
      nextSpec.sampleRate = 44100;
      nextSpec.channelMask = 0x3;
    }

    SECTION("a new count remains different storage")
    {
      nextSpec.channels = 1;
      nextSpec.channelMask = 0x4;
      nextSpec.audioData = {0x35, 0x36};
    }

    auto const nextBytes = ao::test::wav::makeWav(nextSpec);
    auto const next = ao::test::TempFile{nextBytes, ".wav"};
    auto engine = Engine{std::make_unique<PermissiveModeBackend>(), makeEngineTestDevice()};
    engine.play(makePlaybackItem(first.path));
    REQUIRE(engine.status().transport == Transport::Playing);
    auto const preparedRes = engine.setNext(makePlaybackItem(next.path));
    REQUIRE(preparedRes);
    CHECK(preparedRes->transition == Engine::PreparedTransitionMode::DrainFallback);
  }

  TEST_CASE("Engine - successor precision remains bounded by the confirmed endpoint behind a wider client",
            "[audio][integration][engine][gapless][concurrency]")
  {
    auto const firstBytes = ao::test::wav::makeWav({.channels = 2, .audioData = {0x11, 0x12, 0x23, 0x24}});
    auto const first = ao::test::TempFile{firstBytes, ".wav"};
    auto const nextBits = GENERATE(std::uint16_t{16}, std::uint16_t{24});
    auto const nextBytes = ao::test::wav::makeWav(
      {.channels = 2,
       .bitsPerSample = nextBits,
       .audioData = nextBits == 16U ? std::vector<std::uint8_t>{0x35, 0x36, 0x47, 0x48, 0x59, 0x5a, 0x6b, 0x6c}
                                    : std::vector<std::uint8_t>{0x35, 0x36, 0x37, 0x47, 0x48, 0x49}});
    auto const next = ao::test::TempFile{nextBytes, ".wav"};
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

      for (std::size_t i = 0; i < expected.size(); ++i)
      {
        CHECK(output[i] == static_cast<std::byte>(expected[i]));
      }

      CHECK(std::ranges::count(backend->events(), std::string{"open"}, &FakeCapturingBackend::Event::name) == 1);
      auto const thirdBytes =
        ao::test::wav::makeWav({.channels = 2, .bitsPerSample = 24, .audioData = {0x51, 0x52, 0x53, 0x61, 0x62, 0x63}});
      auto const third = ao::test::TempFile{thirdBytes, ".wav"};
      auto const thirdPreparedRes = engine.setNext(makePlaybackItem(third.path));
      REQUIRE(thirdPreparedRes);
      // Promotion changes the source, never the endpoint bound for this open.
      CHECK(thirdPreparedRes->transition == Engine::PreparedTransitionMode::DrainFallback);
    }
  }
} // namespace ao::audio::test
