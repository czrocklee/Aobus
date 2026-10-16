// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EngineTestSupport.h"
#include "FakeCapturingBackend.h"
#include "FlacFrameTestSupport.h"
#include "lib/audio/NullBackend.h"
#include "lib/audio/detail/TrackPreparation.h"
#include "test/unit/TestFixtureSupport.h"
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

  TEST_CASE("Engine - real WAV and FLAC splice only with a backend-proved stereo transport",
            "[audio][integration][engine][gapless][concurrency]")
  {
    bool const wavFirst = GENERATE(false, true);
    bool const permitsReuse = GENERATE(false, true);
    CAPTURE(wavFirst, permitsReuse);
    auto const flac = ao::test::TempFile{stereoFlacBytes(), ".flac"};
    auto const wavPcm = std::vector<std::uint8_t>{0x35, 0x36, 0x47, 0x48, 0x59, 0x5A, 0x6B, 0x6C};
    auto const wavBytes = ao::test::wav::makeWav({.channels = 2, .sampleRate = 48000, .audioData = wavPcm});
    auto const wav = ao::test::TempFile{wavBytes, ".wav"};
    auto const flacPcm = stereoFlacPcm();
    auto const& firstPcm = wavFirst ? wavPcm : flacPcm;
    auto const& secondPcm = wavFirst ? flacPcm : wavPcm;
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
    auto const& firstPath = wavFirst ? wav.path : flac.path;
    auto const& secondPath = wavFirst ? flac.path : wav.path;
    engine.play(makePlaybackItem(firstPath));
    REQUIRE(engine.status().transport == Transport::Playing);
    auto const initialMask = wavFirst ? 0x3U : 0U;
    auto const nextMask = wavFirst ? 0U : 0x3U;
    CHECK(backend->currentFormat().speakerMask == initialMask);
    CHECK(engine.status().routeState.sourceFormat.speakerMask == initialMask);
    auto const preparedRes = engine.setNext(makePlaybackItem(secondPath));
    REQUIRE(preparedRes);
    CHECK(preparedRes->transition ==
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
      CHECK(status.routeState.sourceChannelLayout.origin ==
            (wavFirst ? ChannelLayoutOrigin::Unknown : ChannelLayoutOrigin::FormatDefault));
      // Native configuration is not rewritten to fabricate a new observation.
      CHECK(backend->currentFormat().speakerMask == initialMask);
      CHECK(std::ranges::count(backend->events(), std::string{"open"}, &FakeCapturingBackend::Event::name) == 1);
      CHECK(std::ranges::count(backend->events(), std::string{"start"}, &FakeCapturingBackend::Event::name) == 1);

      // Reuse again in the opposite direction after transition publication:
      // the installed native mode remains the original one throughout.
      auto const thirdRes = engine.setNext(makePlaybackItem(firstPath));
      REQUIRE(thirdRes);
      CHECK(thirdRes->transition == Engine::PreparedTransitionMode::Gapless);
      auto thirdExpected = std::vector<std::uint8_t>{secondTail.begin(), secondTail.end()};
      thirdExpected.insert(thirdExpected.end(), firstPcm.begin(), firstPcm.end());
      auto thirdOutput = std::vector<std::byte>(thirdExpected.size());
      auto const thirdRendered = target->renderPcm(thirdOutput);
      REQUIRE(thirdRendered.bytesWritten == thirdOutput.size());
      target->handlePositionAdvanced(thirdRendered.positionFrames);
      CHECK(std::ranges::equal(thirdOutput, utility::bytes::view(std::span<std::uint8_t const>{thirdExpected})));
      REQUIRE(advanced.tryWaitForCount(2));
      CHECK(engine.status().routeState.sourceFormat.speakerMask == initialMask);
      CHECK(engine.status().routeState.engineOutputFormat.speakerMask == initialMask);
      CHECK(backend->currentFormat().speakerMask == initialMask);
      CHECK(std::ranges::count(backend->events(), std::string{"open"}, &FakeCapturingBackend::Event::name) == 1);
      CHECK(std::ranges::count(backend->events(), std::string{"start"}, &FakeCapturingBackend::Event::name) == 1);
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
      CHECK(engine.status().transport == Transport::Idle);
      engine.play(makePlaybackItem(secondPath));
      REQUIRE(engine.status().transport == Transport::Playing);
      CHECK(backend->currentFormat().speakerMask == nextMask);
      CHECK(std::ranges::count(backend->events(), std::string{"open"}, &FakeCapturingBackend::Event::name) == 2);
    }
  }

  TEST_CASE("Engine - stereo transport proof is rechecked after decoder preparation",
            "[audio][integration][engine][gapless][concurrency]")
  {
    auto const flac = ao::test::TempFile{stereoFlacBytes(), ".flac"};
    auto const wavBytes =
      ao::test::wav::makeWav({.channels = 2, .sampleRate = 48000, .audioData = {0x35, 0x36, 0x47, 0x48}});
    auto const wav = ao::test::TempFile{wavBytes, ".wav"};
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    backend->setStereoModeReuseAllowed(true);
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice()};
    engine.play(makePlaybackItem(flac.path));
    REQUIRE(engine.status().transport == Transport::Playing);
    auto preparationRes = detail::TrackPreparation::capture(engine,
                                                            makePlaybackItem(wav.path),
                                                            std::chrono::milliseconds{0},
                                                            detail::TrackPreparation::Purpose::GaplessLookahead);
    REQUIRE(preparationRes);
    REQUIRE(preparationRes->inspect());
    REQUIRE(preparationRes->selectPrewarmFormat(engine));
    REQUIRE(preparationRes->prepare());
    backend->setStereoModeReuseAllowed(false);
    auto const adoptedRes = std::move(*preparationRes).adoptNext(engine);
    REQUIRE(adoptedRes);
    CHECK(adoptedRes->transition == Engine::PreparedTransitionMode::DrainFallback);
    auto* const target = backend->target();
    REQUIRE(target != nullptr);
    auto output = std::array<std::byte, 68>{};
    auto const expected = stereoFlacPcm();
    auto const rendered = target->renderPcm(output);
    CHECK(rendered.bytesWritten == 64U);
    CHECK_FALSE(rendered.drained);
    CHECK(
      std::ranges::equal(std::span{output}.first(64), utility::bytes::view(std::span<std::uint8_t const>{expected})));
    auto finalOutput = std::array<std::byte, 4>{};
    auto const drained = target->renderPcm(finalOutput);
    REQUIRE(drained.bytesWritten == 0U);
    REQUIRE(drained.drained);
    CHECK(engine.status().routeState.sourceFormat.speakerMask == 0U);
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
