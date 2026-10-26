// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EngineTestSupport.h"
#include "FakeCapturingBackend.h"
#include "test/TestFixtureSupport.h"
#include "test/media/wav/TestWav.h"
#include <ao/audio/Engine.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/Transport.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ao::audio::test
{
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
