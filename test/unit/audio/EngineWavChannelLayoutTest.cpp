// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EngineFailureTestSupport.h"
#include "EngineTestSupport.h"
#include "FakeCapturingBackend.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/media/wav/TestWav.h"
#include <ao/Error.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/Engine.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/Transport.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  TEST_CASE("Engine - real WAV layouts control successor reuse without rewriting canonical samples",
            "[audio][integration][engine][wav]")
  {
    // First track: plain quad is inferred FL FR BL BR. These complete S16LE
    // frames and the successor frames are independently authored slot markers.
    auto const firstPcm = std::vector<std::uint8_t>{
      0x11,
      0x12,
      0x21,
      0x22,
      0x31,
      0x32,
      0x41,
      0x42,
      0x51,
      0x52,
      0x61,
      0x62,
      0x71,
      0x72,
      0x81,
      0x82,
    };
    auto const secondPcm = std::vector<std::uint8_t>{
      0x91,
      0x92,
      0xA1,
      0xA2,
      0xB1,
      0xB2,
      0xC1,
      0xC2,
      0xD1,
      0xD2,
      0xE1,
      0xE2,
      0xF1,
      0xF2,
      0x01,
      0x02,
    };
    auto const firstBytes = ao::test::wav::makeWav({.channels = 4, .audioData = firstPcm});
    auto const firstFile = ao::test::TempFile{firstBytes, ".wav"};
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice()};
    auto advanced = CallbackLatch{};
    auto ended = CallbackLatch{};
    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advanced.notify(); });
    engine.setOnTrackEnded([&](Engine::TrackEnded const&) { ended.notify(); });

    std::uint32_t nextMask = 0x33;

    SECTION("equal effective identity splices and refreshes inferred to declared history")
    {
    }

    SECTION("same count but FL FR FC BC identity drains and reopens")
    {
      nextMask = 0x107;
    }

    auto const secondBytes = ao::test::wav::makeWav({.sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
                                                     .channels = 4,
                                                     .channelMask = nextMask,
                                                     .audioData = secondPcm});
    auto const secondFile = ao::test::TempFile{secondBytes, ".wav"};
    engine.play(makePlaybackItem(firstFile.path));
    REQUIRE(engine.status().transport == Transport::Playing);
    CHECK(engine.status().routeState.sourceFormat.speakerMask == 0x33);
    CHECK(engine.status().routeState.sourceChannelLayout.origin == ChannelLayoutOrigin::Inferred);
    CHECK_FALSE(engine.status().routeState.sourceChannelLayout.optDeclaredMask);
    CHECK(backend->currentFormat().speakerMask == 0x33);

    auto const preparedRes = engine.setNext(makePlaybackItem(secondFile.path));
    REQUIRE(preparedRes);
    CHECK(preparedRes->transition ==
          (nextMask == 0x33 ? Engine::PreparedTransitionMode::Gapless : Engine::PreparedTransitionMode::DrainFallback));
    auto* const firstTarget = backend->target();
    REQUIRE(firstTarget != nullptr);
    auto output = std::array<std::byte, 16>{};
    REQUIRE(firstTarget->renderPcm(output).bytesWritten == output.size());
    CHECK(std::ranges::equal(output, utility::bytes::view(std::span<std::uint8_t const>{firstPcm})));

    if (nextMask == 0x33)
    {
      REQUIRE(firstTarget->renderPcm(output).bytesWritten == output.size());
      CHECK(std::ranges::equal(output, utility::bytes::view(std::span<std::uint8_t const>{secondPcm})));
      REQUIRE(advanced.tryWaitForCount(1));
      CHECK(ended.count() == 0);
      CHECK(engine.status().routeState.sourceChannelLayout ==
            ChannelLayoutEvidence{.origin = ChannelLayoutOrigin::Declared, .optDeclaredMask = 0x33});
    }
    else
    {
      auto const drain = firstTarget->renderPcm(output);
      CHECK(drain.drained);
      CHECK(drain.bytesWritten == 0);
      CHECK(advanced.count() == 0);
      backend->emitDrainComplete();
      REQUIRE(ended.tryWaitForCount(1));
      CHECK(engine.status().transport == Transport::Idle);
      engine.play(makePlaybackItem(secondFile.path));
      REQUIRE(engine.status().transport == Transport::Playing);
      auto* const reopenedTarget = backend->target();
      REQUIRE(reopenedTarget != nullptr);
      REQUIRE(reopenedTarget->renderPcm(output).bytesWritten == output.size());
      CHECK(std::ranges::equal(output, utility::bytes::view(std::span<std::uint8_t const>{secondPcm})));
      CHECK(engine.status().routeState.sourceChannelLayout ==
            ChannelLayoutEvidence{.origin = ChannelLayoutOrigin::Declared, .optDeclaredMask = 0x107});
    }

    auto const status = engine.status();
    CHECK(status.routeState.sourceFormat.speakerMask == nextMask);
    CHECK(status.routeState.decoderOutputFormat.speakerMask == nextMask);
    CHECK(status.routeState.engineOutputFormat.speakerMask == nextMask);
    CHECK(backend->currentFormat().speakerMask == nextMask);
    auto const events = backend->events();
    CHECK(std::ranges::count(events, std::string{"open"}, &FakeCapturingBackend::Event::name) ==
          (nextMask == 0x33 ? 1 : 2));
    CHECK(std::ranges::count(events, std::string{"start"}, &FakeCapturingBackend::Event::name) ==
          (nextMask == 0x33 ? 1 : 2));
  }

  TEST_CASE("Engine - real partial WAV declaration is a recoverable source refusal before backend open",
            "[audio][integration][engine][wav]")
  {
    auto const bytes = ao::test::wav::makeWav({.sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
                                               .channels = 2,
                                               .channelMask = 0x1,
                                               .audioData = {0x11, 0x12, 0x21, 0x22}});
    auto const file = ao::test::TempFile{bytes, ".wav"};
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice()};
    auto failureFuture = captureNextFailure(engine);
    auto const item = makePlaybackItem(file.path);
    engine.play(item);
    auto const failure = requireFailure(failureFuture);
    CHECK(failure.itemId == item.id);
    CHECK(failure.kind == Engine::PlaybackFailureKind::TrackOpen);
    CHECK(failure.recoverable);
    CHECK(failure.error.code == Error::Code::NotSupported);
    CHECK(failure.error.message.contains("WAV channel layout"));
    CHECK(failure.error.message.contains("partially"));
    auto const events = backend->events();
    CHECK(std::ranges::count(events, std::string{"open"}, &FakeCapturingBackend::Event::name) == 0);
    CHECK(std::ranges::count(events, std::string{"start"}, &FakeCapturingBackend::Event::name) == 0);
  }
} // namespace ao::audio::test
