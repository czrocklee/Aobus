// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/AlsaExclusiveBackend.h"

#include "lib/audio/backend/detail/AlsaGraphRegistry.h"
#include "test/unit/audio/BackendTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Property.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <tuple>

namespace ao::audio::backend::test
{
  TEST_CASE("AlsaExclusiveBackend - non-hardware PCM is rejected", "[audio][regression][alsa]")
  {
    auto target = ::ao::audio::test::NoopRenderTarget{};
    auto const device = Device{
      .id = DeviceId{"null"}, .displayName = "ALSA null plugin", .description = "null", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive};

    auto const openedRes = backend.open(SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 24}, target);

    REQUIRE_FALSE(openedRes);
    CHECK(openedRes.error().code == Error::Code::FormatRejected);
    CHECK(openedRes.error().message.contains("direct hardware PCM"));
  }

  TEST_CASE("AlsaExclusiveBackend - unsupported signal is rejected before native device inspection",
            "[audio][regression][alsa]")
  {
    auto target = ::ao::audio::test::NoopRenderTarget{};
    auto const device = Device{
      .id = DeviceId{"null"}, .displayName = "ALSA null plugin", .description = "null", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive};

    auto const openedRes = backend.open(SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 33}, target);

    REQUIRE_FALSE(openedRes);
    CHECK(openedRes.error().code == Error::Code::NotSupported);
    CHECK(openedRes.error().message == "No lossless PCM encoding is available for ALSA");
  }

  TEST_CASE("AlsaExclusiveBackend - a missing device is reported as such, never as a format problem",
            "[audio][regression][alsa]")
  {
    auto target = ::ao::audio::test::NoopRenderTarget{};
    // A failed native open must surface its own cause. Translating it into a
    // format decision is exactly how an unavailable device once turned into a
    // silent 16-bit fallback.
    auto const device = Device{
      .id = DeviceId{"hw:127,0"}, .displayName = "Absent card", .description = "hw:127,0", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive};

    auto const openedRes = backend.open(SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 24}, target);

    REQUIRE_FALSE(openedRes);
    CHECK(openedRes.error().code != Error::Code::FormatRejected);
    CHECK(openedRes.error().message.contains("Failed to open ALSA device"));
  }

  TEST_CASE("AlsaExclusiveBackend - a hint is never treated as an opened mode", "[audio][regression][alsa]")
  {
    auto target = ::ao::audio::test::NoopRenderTarget{};
    // Nothing is cached before a successful open, so a device that was never
    // opened predicts only from immutable policy.
    auto const device = Device{
      .id = DeviceId{"hw:127,0"}, .displayName = "Absent card", .description = "hw:127,0", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive};
    auto const sourceFormat = SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 24};

    std::ignore = backend.open(sourceFormat, target);

    auto const optHint = backend.prewarmFormatHint(sourceFormat);

    REQUIRE(optHint);
    CHECK(optHint->encoding == SampleEncoding::Signed24PackedLe);
  }

  TEST_CASE("AlsaExclusiveBackend - retained publisher is inert after graph retirement",
            "[audio][regression][alsa][concurrency]")
  {
    auto registry = detail::AlsaGraphRegistry{};
    auto graph = flow::Graph{};
    std::size_t graphUpdateCount = 0U;
    auto graphSub = registry.subscribe("hw:127,0",
                                       [&](flow::Graph const& nextGraph)
                                       {
                                         graph = nextGraph;
                                         ++graphUpdateCount;
                                       });
    auto const device = Device{
      .id = DeviceId{"hw:127,0"}, .displayName = "Absent card", .description = "hw:127,0", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive, registry.publisher()};

    REQUIRE(backend.set(props::kVolume, 0.5F));
    REQUIRE(graphUpdateCount == 2U);
    REQUIRE_FALSE(graph.nodes.empty());

    registry.shutdown();
    REQUIRE(graphUpdateCount == 3U);
    CHECK(graph.nodes.empty());

    REQUIRE(backend.set(props::kVolume, 0.25F));
    backend.close();
    CHECK(graphUpdateCount == 3U);
    CHECK(graph.nodes.empty());
  }
} // namespace ao::audio::backend::test
