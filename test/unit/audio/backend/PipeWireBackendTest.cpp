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
#include <catch2/generators/catch_generators.hpp>

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

  TEST_CASE("PipeWireBackend - geometry refusal outranks precision refusal", "[audio][unit][pipewire]")
  {
    auto backend = PipeWireBackend{syntheticSink(), kProfileShared};
    auto target = ::ao::audio::test::NoopRenderTarget{};

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
      auto const sourceFormat = SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 33};
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

  TEST_CASE("PipeWireBackend - identified source roles are refused before native channel admission is available",
            "[audio][unit][pipewire]")
  {
    auto const profile = GENERATE(kProfileShared, kProfileExclusive);
    auto backend = PipeWireBackend{syntheticSink(), profile};
    auto target = ::ao::audio::test::NoopRenderTarget{};
    // Neither an alternate back pair nor canonical FL/FR is admitted by count alone.
    auto const speakerMask = GENERATE(0x30U, 0x3U);
    auto const sourceFormat =
      SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = speakerMask};

    CHECK_FALSE(backend.prewarmFormatHint(sourceFormat));
    auto const openedRes = backend.open(sourceFormat, target);

    REQUIRE_FALSE(openedRes);
    CHECK(openedRes.error().error.code == Error::Code::FormatRejected);
    CHECK(openedRes.error().error.message == "PipeWire source speaker identity is not supported");
    CHECK(openedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
  }

  TEST_CASE("PipeWireBackend - prewarm predicts the lossless mode without native negotiation",
            "[audio][unit][pipewire]")
  {
    // prewarmFormatHint is non-blocking and performs no native open or
    // negotiation, so a synthetic device observes only local admission policy.
    auto backend = PipeWireBackend{syntheticSink(), kProfileShared};

    SECTION("an ordinary representable source retains its precision")
    {
      auto const optHint =
        backend.prewarmFormatHint(SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16});

      REQUIRE(optHint);
      CHECK(optHint->sampleRate == 48000U);
      CHECK(optHint->channels == 2U);
      CHECK(optHint->encoding == SampleEncoding::Signed16Le);
    }
  }
} // namespace ao::audio::backend::test
