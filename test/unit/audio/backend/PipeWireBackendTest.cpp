// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/PipeWireBackend.h"

#include "test/unit/audio/BackendTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace ao::audio::backend::test
{
  namespace
  {
    // The backend constructor may initialize PipeWire runtime state, but no
    // test here reaches native stream work: every open() below is refused by
    // the deterministic source-admission guard that precedes all native calls.
    Device syntheticSink()
    {
      return {.id = DeviceId{"synthetic-sink"},
              .displayName = "Synthetic PipeWire Sink",
              .description = "Guard-only sink",
              .backendId = kBackendPipeWire};
    }
  } // namespace

  TEST_CASE("PipeWireBackend - zero geometry is refused as invalid input before native stream work",
            "[audio][unit][pipewire]")
  {
    auto backend = PipeWireBackend{syntheticSink(), kProfileShared};
    auto target = ::ao::audio::test::NoopRenderTarget{};

    SECTION("a zero sample rate is invalid geometry")
    {
      auto const sourceFormat = SignalFormat{.sampleRate = 0, .channels = 2, .precisionBits = 16};
      auto const openedRes = backend.open(sourceFormat, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "PipeWire PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK_FALSE(backend.prewarmFormatHint(sourceFormat).has_value());
    }

    SECTION("a zero channel count is invalid geometry")
    {
      auto const sourceFormat = SignalFormat{.sampleRate = 48000, .channels = 0, .precisionBits = 16};
      auto const openedRes = backend.open(sourceFormat, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "PipeWire PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK_FALSE(backend.prewarmFormatHint(sourceFormat).has_value());
    }
  }

  TEST_CASE("PipeWireBackend - geometry refusal outranks layout and precision refusals", "[audio][unit][pipewire]")
  {
    auto backend = PipeWireBackend{syntheticSink(), kProfileShared};
    auto target = ::ao::audio::test::NoopRenderTarget{};

    SECTION("a masked source with no sample rate still reports the geometry refusal")
    {
      auto const openedRes =
        backend.open(SignalFormat{.sampleRate = 0, .channels = 2, .precisionBits = 16, .speakerMask = 0x3}, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "PipeWire PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    }

    SECTION("an unrepresentable source with no channels still reports the geometry refusal")
    {
      auto const openedRes =
        backend.open(SignalFormat{.sampleRate = 48000, .channels = 0, .precisionBits = 33}, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "PipeWire PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    }
  }

  TEST_CASE("PipeWireBackend - an unrepresentable source is refused as a track incompatibility",
            "[audio][unit][pipewire]")
  {
    auto backend = PipeWireBackend{syntheticSink(), kProfileShared};
    auto target = ::ao::audio::test::NoopRenderTarget{};

    SECTION("a 33-bit integer signal has no lossless encoding at all")
    {
      auto const sourceFormat =
        SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 33, .speakerMask = 0x30};
      auto const openedRes = backend.open(sourceFormat, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::NotSupported);
      CHECK(openedRes.error().error.message == "No lossless PCM encoding is available for PipeWire");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
      CHECK_FALSE(backend.prewarmFormatHint(sourceFormat).has_value());
    }

    SECTION("a non-32-bit float signal has no lossless encoding at all")
    {
      auto const sourceFormat =
        SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::FloatingPoint};
      auto const openedRes = backend.open(sourceFormat, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::NotSupported);
      CHECK(openedRes.error().error.message == "No lossless PCM encoding is available for PipeWire");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
      CHECK_FALSE(backend.prewarmFormatHint(sourceFormat).has_value());
    }
  }

  TEST_CASE("PipeWireBackend - prewarm retains admitted complete sets without native negotiation",
            "[audio][unit][pipewire]")
  {
    // prewarmFormatHint is non-blocking and performs no native open or
    // negotiation, so a synthetic device observes only local admission policy.
    auto backend = PipeWireBackend{syntheticSink(), kProfileShared};

    for (auto const source :
         {SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16},
          SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 0x3},
          SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 0x30},
          SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 0x5},
          SignalFormat{.sampleRate = 48000, .channels = 1, .precisionBits = 16, .speakerMask = 0x1},
          SignalFormat{.sampleRate = 48000, .channels = 1, .precisionBits = 16, .speakerMask = 0x4},
          SignalFormat{.sampleRate = 48000, .channels = 4, .precisionBits = 16, .speakerMask = 0x33}})
    {
      auto const optHint = backend.prewarmFormatHint(source);
      REQUIRE(optHint);
      CHECK(optHint->sampleRate == 48000U);
      CHECK(optHint->channels == source.channels);
      CHECK(optHint->speakerMask == source.speakerMask);
      CHECK(optHint->encoding == SampleEncoding::Signed16Le);
    }
  }

  TEST_CASE("PipeWireBackend - malformed speaker sets are local track refusals", "[audio][unit][pipewire]")
  {
    auto backend = PipeWireBackend{syntheticSink(), kProfileShared};
    auto target = ::ao::audio::test::NoopRenderTarget{};

    for (auto const mask : {std::uint32_t{1}, std::uint32_t{7}, std::uint32_t{0x80000001}})
    {
      auto const source = SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = mask};
      auto const openedRes = backend.open(source, target);
      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
      CHECK(openedRes.error().error.code == Error::Code::NotSupported);
      CHECK(openedRes.error().error.message == "PipeWire requires a complete supported speaker set");
      CHECK_FALSE(backend.prewarmFormatHint(source));
    }
  }
} // namespace ao::audio::backend::test
