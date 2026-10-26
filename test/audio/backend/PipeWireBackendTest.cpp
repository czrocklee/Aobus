// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/PipeWireBackend.h"

#include "test/audio/BackendTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace ao::audio::backend::test
{
  namespace
  {
    // Construction may initialize/connect PipeWire runtime state. The reuse
    // query itself performs no I/O and needs no successful connection or open.
    // Every open() below is refused by the deterministic source-admission
    // guard before native stream work.
    Device syntheticSink()
    {
      return {.id = DeviceId{"synthetic-sink"},
              .displayName = "Synthetic PipeWire Sink",
              .description = "Guard-only sink",
              .backendId = kBackendPipeWire};
    }
  } // namespace

  TEST_CASE("PipeWireBackend - reuse query preserves exact identity and only admits bounded stereo aliases",
            "[audio][unit][pipewire]")
  {
    struct ReuseCase final
    {
      std::string_view name;
      PcmFormat first;
      PcmFormat second;
      bool expected;
    };

    auto const makeFormat =
      [](std::uint32_t sampleRate, std::uint8_t channels, SampleEncoding encoding, std::uint32_t speakerMask)
    {
      return PcmFormat{
        .sampleRate = sampleRate, .channels = channels, .encoding = encoding, .speakerMask = speakerMask};
    };
    auto const cases = std::array{
      ReuseCase{.name = "exact valid identified S16 stereo",
                .first = makeFormat(48000, 2, SampleEncoding::Signed16Le, 0x3),
                .second = makeFormat(48000, 2, SampleEncoding::Signed16Le, 0x3),
                .expected = true},
      // Exact equality is not a global format-validation gate.
      ReuseCase{.name = "exact empty default", .first = PcmFormat{}, .second = PcmFormat{}, .expected = true},
      ReuseCase{.name = "S16 stereo identity alias",
                .first = makeFormat(48000, 2, SampleEncoding::Signed16Le, 0),
                .second = makeFormat(48000, 2, SampleEncoding::Signed16Le, 0x3),
                .expected = true},
      ReuseCase{.name = "packed S24 stereo identity alias",
                .first = makeFormat(48000, 2, SampleEncoding::Signed24PackedLe, 0),
                .second = makeFormat(48000, 2, SampleEncoding::Signed24PackedLe, 0x3),
                .expected = true},
      ReuseCase{.name = "S24 in S32 stereo identity alias",
                .first = makeFormat(48000, 2, SampleEncoding::Signed24In32Le, 0),
                .second = makeFormat(48000, 2, SampleEncoding::Signed24In32Le, 0x3),
                .expected = true},
      ReuseCase{.name = "S32 stereo identity alias",
                .first = makeFormat(48000, 2, SampleEncoding::Signed32Le, 0),
                .second = makeFormat(48000, 2, SampleEncoding::Signed32Le, 0x3),
                .expected = true},
      ReuseCase{.name = "float32 stereo identity alias",
                .first = makeFormat(48000, 2, SampleEncoding::Float32Le, 0),
                .second = makeFormat(48000, 2, SampleEncoding::Float32Le, 0x3),
                .expected = true},
      ReuseCase{.name = "different rate",
                .first = makeFormat(48000, 2, SampleEncoding::Signed16Le, 0),
                .second = makeFormat(96000, 2, SampleEncoding::Signed16Le, 0x3),
                .expected = false},
      ReuseCase{.name = "different channel count",
                .first = makeFormat(48000, 2, SampleEncoding::Signed16Le, 0),
                .second = makeFormat(48000, 1, SampleEncoding::Signed16Le, 0x3),
                .expected = false},
      ReuseCase{.name = "different encoding",
                .first = makeFormat(48000, 2, SampleEncoding::Signed16Le, 0),
                .second = makeFormat(48000, 2, SampleEncoding::Signed24PackedLe, 0x3),
                .expected = false},
      ReuseCase{.name = "back stereo roles are not a wildcard match",
                .first = makeFormat(48000, 2, SampleEncoding::Signed16Le, 0),
                .second = makeFormat(48000, 2, SampleEncoding::Signed16Le, 0x30),
                .expected = false},
      ReuseCase{.name = "front-left and front-center roles are not a wildcard match",
                .first = makeFormat(48000, 2, SampleEncoding::Signed16Le, 0),
                .second = makeFormat(48000, 2, SampleEncoding::Signed16Le, 0x5),
                .expected = false},
      ReuseCase{.name = "front-center mono is not a stereo alias",
                .first = makeFormat(48000, 1, SampleEncoding::Signed16Le, 0),
                .second = makeFormat(48000, 1, SampleEncoding::Signed16Le, 0x4),
                .expected = false},
      ReuseCase{.name = "four-channel speaker set is not a stereo alias",
                .first = makeFormat(48000, 4, SampleEncoding::Signed16Le, 0),
                .second = makeFormat(48000, 4, SampleEncoding::Signed16Le, 0x33),
                .expected = false},
      ReuseCase{.name = "zero channels cannot form a stereo alias",
                .first = makeFormat(48000, 0, SampleEncoding::Signed16Le, 0),
                .second = makeFormat(48000, 0, SampleEncoding::Signed16Le, 0x3),
                .expected = false},
      ReuseCase{.name = "zero rate cannot form a stereo alias",
                .first = makeFormat(0, 2, SampleEncoding::Signed16Le, 0),
                .second = makeFormat(0, 2, SampleEncoding::Signed16Le, 0x3),
                .expected = false},
      ReuseCase{.name = "unknown encoding cannot form a stereo alias",
                .first = makeFormat(48000, 2, SampleEncoding::Unknown, 0),
                .second = makeFormat(48000, 2, SampleEncoding::Unknown, 0x3),
                .expected = false},
      ReuseCase{.name = "unassigned encoding cannot form a stereo alias",
                .first = makeFormat(48000, 2, static_cast<SampleEncoding>(0xff), 0),
                .second = makeFormat(48000, 2, static_cast<SampleEncoding>(0xff), 0x3),
                .expected = false}};
    auto backend = PipeWireBackend{syntheticSink(), kProfileShared};
    Backend const& dispatch = backend;

    for (auto const& row : cases)
    {
      CAPTURE(row.name);
      CHECK(backend.canReuseFormat(row.first, row.second) == row.expected);
      CHECK(backend.canReuseFormat(row.second, row.first) == row.expected);
      CHECK(dispatch.canReuseFormat(row.first, row.second) == row.expected);
      CHECK(dispatch.canReuseFormat(row.second, row.first) == row.expected);
    }
  }

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

    SECTION("an unsupported precision with a partial speaker set still reports the precision refusal")
    {
      // The caller is invalid in both dimensions: 33-bit precision has no
      // lossless encoding and a 0x1 mask does not identify both channels.
      // The precision rung owns the refusal.
      auto const sourceFormat =
        SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 33, .speakerMask = 0x1};
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
