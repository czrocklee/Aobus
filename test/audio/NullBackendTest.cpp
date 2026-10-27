// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/NullBackend.h"

#include "BackendTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>
#include <type_traits>

namespace ao::audio::test
{
  static_assert(std::is_nothrow_default_constructible_v<NullBackend>);

  TEST_CASE("NullBackend - lifecycle is inert and identifies the shared null route")
  {
    auto target = NoopRenderTarget{};
    auto backend = NullBackend{};
    auto const sourceFormat = SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 16};

    auto const optHint = backend.prewarmFormatHint(sourceFormat);
    REQUIRE(optHint);
    CHECK(*optHint == pcmFormat(sourceFormat, SampleEncoding::Signed16Le));

    auto const openRes = backend.open(sourceFormat, target);
    REQUIRE(openRes);
    CHECK(openRes->clientFormat == pcmFormat(sourceFormat, SampleEncoding::Signed16Le));
    // A discarding sink has no direct endpoint evidence, but its client mode
    // still preserves the source precision.
    CHECK_FALSE(openRes->optEndpoint);
    backend.start();
    backend.pause();
    backend.resume();
    backend.flush();
    backend.stop();
    backend.close();

    CHECK(backend.backendId() == kBackendNone);
    CHECK(backend.profileId() == kProfileShared);
  }

  TEST_CASE("NullBackend - retains identified application slots without claiming a physical endpoint")
  {
    auto target = NoopRenderTarget{};
    auto backend = NullBackend{};
    // A one-channel FL identity is not automatically relabelled centered mono.
    auto const source = SignalFormat{.sampleRate = 48000, .channels = 1, .precisionBits = 16, .speakerMask = 0x1};
    auto const expected =
      PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x1};

    auto const optHint = backend.prewarmFormatHint(source);
    REQUIRE(optHint);
    CHECK(*optHint == expected);

    auto const openedRes = backend.open(source, target);
    REQUIRE(openedRes);
    CHECK(openedRes->clientFormat == expected);
    CHECK(frameBytes(openedRes->clientFormat) == 2);
    CHECK_FALSE(openedRes->optEndpoint);
  }

  TEST_CASE("NullBackend - reuse preserves exact equality without requiring an open")
  {
    auto backend = NullBackend{};
    Backend const& dispatch = backend;
    auto const format =
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x3};

    CHECK(backend.canReuseFormat(format, format));
    CHECK(dispatch.canReuseFormat(format, format));
    // Exact equality is not a global format-validation gate.
    CHECK(backend.canReuseFormat(PcmFormat{}, PcmFormat{}));
  }

  TEST_CASE("NullBackend - reuse accepts bidirectional stereo identity aliases in every encoding")
  {
    auto backend = NullBackend{};
    Backend const& dispatch = backend;
    auto const encoding = GENERATE(SampleEncoding::Signed16Le,
                                   SampleEncoding::Signed24PackedLe,
                                   SampleEncoding::Signed24In32Le,
                                   SampleEncoding::Signed32Le,
                                   SampleEncoding::Float32Le);
    auto const unidentified = PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = encoding};
    auto identified = unidentified;
    identified.speakerMask = 0x3;

    CHECK(backend.canReuseFormat(unidentified, identified));
    CHECK(backend.canReuseFormat(identified, unidentified));
    CHECK(dispatch.canReuseFormat(unidentified, identified));
    CHECK(dispatch.canReuseFormat(identified, unidentified));
  }

  TEST_CASE("NullBackend - reuse refuses changed PCM geometry rate or encoding")
  {
    auto backend = NullBackend{};
    auto const first = PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le};
    auto second = first;
    second.speakerMask = 0x3;

    SECTION("different rate")
    {
      second.sampleRate = 96000;
    }

    SECTION("different channel count")
    {
      second.channels = 1;
    }

    SECTION("different encoding")
    {
      second.encoding = SampleEncoding::Signed24PackedLe;
    }

    CHECK_FALSE(backend.canReuseFormat(first, second));
    CHECK_FALSE(backend.canReuseFormat(second, first));
  }

  TEST_CASE("NullBackend - unidentified masks are not a reuse wildcard for other roles or channel counts")
  {
    auto backend = NullBackend{};
    auto unidentified = PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le};
    std::uint32_t mask = 0x30;

    SECTION("back stereo roles")
    {
      mask = 0x30;
    }

    SECTION("front-left and front-center roles")
    {
      mask = 0x5;
    }

    SECTION("front-center mono")
    {
      unidentified.channels = 1;
      mask = 0x4;
    }

    SECTION("four-channel speaker set")
    {
      unidentified.channels = 4;
      mask = 0x33;
    }

    SECTION("zero channels")
    {
      unidentified.channels = 0;
      mask = 0x3;
    }

    auto identified = unidentified;
    identified.speakerMask = mask;

    CHECK_FALSE(backend.canReuseFormat(unidentified, identified));
    CHECK_FALSE(backend.canReuseFormat(identified, unidentified));
  }

  TEST_CASE("NullBackend - stereo aliases with invalid rate or encoding cannot be reused")
  {
    auto backend = NullBackend{};
    auto unidentified = PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le};

    SECTION("zero rate")
    {
      unidentified.sampleRate = 0;
    }

    SECTION("unknown encoding")
    {
      unidentified.encoding = SampleEncoding::Unknown;
    }

    SECTION("unassigned encoding")
    {
      unidentified.encoding = static_cast<SampleEncoding>(0xff);
    }

    auto identified = unidentified;
    identified.speakerMask = 0x3;

    CHECK_FALSE(backend.canReuseFormat(unidentified, identified));
    CHECK_FALSE(backend.canReuseFormat(identified, unidentified));
  }

  TEST_CASE("NullBackend - source without a lossless PCM encoding is rejected")
  {
    auto target = NoopRenderTarget{};
    auto backend = NullBackend{};
    auto const source = SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 33};

    CHECK_FALSE(backend.prewarmFormatHint(source));

    auto const openedRes = backend.open(source, target);
    REQUIRE_FALSE(openedRes);
    CHECK(openedRes.error().error.code == Error::Code::NotSupported);
    CHECK(openedRes.error().error.message == "No lossless PCM encoding is available");
    // The 33-bit constraint is a source property, so another track may satisfy it.
    CHECK(openedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
  }

  TEST_CASE("NullBackend - zero-precision source stays refused without a prewarm hint")
  {
    auto target = NoopRenderTarget{};
    auto backend = NullBackend{};
    // A zero precision is an unknown source precision; an identified speaker
    // mask does not license an open for it.
    auto const source = SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 0, .speakerMask = 0x3};

    CHECK_FALSE(backend.prewarmFormatHint(source));

    for (std::int32_t attempt = 0; attempt < 2; ++attempt)
    {
      auto const openedRes = backend.open(source, target);
      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::NotSupported);
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    }
  }

  TEST_CASE("NullBackend - zero-geometry source is rejected as invalid input")
  {
    auto target = NoopRenderTarget{};
    auto backend = NullBackend{};
    auto source = SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 16};

    SECTION("zero sample rate")
    {
      source.sampleRate = 0;
    }

    SECTION("zero channel count")
    {
      source.channels = 0;
    }

    CHECK_FALSE(backend.prewarmFormatHint(source));

    auto const openedRes = backend.open(source, target);
    REQUIRE_FALSE(openedRes);
    CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
    CHECK(openedRes.error().error.message == "PCM formats require a sample rate and channels");
    // The request itself is malformed, not a constraint another track satisfies.
    CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
  }

  TEST_CASE("NullBackend - volume and mute properties remain readable and writable")
  {
    auto backend = NullBackend{};

    auto volumeRes = backend.property(PropertyId::Volume);
    REQUIRE(volumeRes);
    CHECK(std::get<float>(*volumeRes) == 1.0F);

    auto mutedRes = backend.property(PropertyId::Muted);
    REQUIRE(mutedRes);
    CHECK_FALSE(std::get<bool>(*mutedRes));

    REQUIRE(backend.setProperty(PropertyId::Volume, PropertyValue{0.25F}));
    REQUIRE(backend.setProperty(PropertyId::Muted, PropertyValue{true}));

    volumeRes = backend.property(PropertyId::Volume);
    REQUIRE(volumeRes);
    CHECK(std::get<float>(*volumeRes) == 0.25F);

    mutedRes = backend.property(PropertyId::Muted);
    REQUIRE(mutedRes);
    CHECK(std::get<bool>(*mutedRes));

    auto const expectedInfo =
      PropertyInfo{.canRead = true, .canWrite = true, .isAvailable = true, .emitsChangeNotifications = false};
    CHECK(backend.queryProperty(PropertyId::Volume) == expectedInfo);
    CHECK(backend.queryProperty(PropertyId::Muted) == expectedInfo);
  }

  TEST_CASE("NullBackend - unknown properties are unsupported")
  {
    auto backend = NullBackend{};
    auto const unknown = static_cast<PropertyId>(UINT8_C(0xff));

    auto const writeRes = backend.setProperty(unknown, PropertyValue{false});
    REQUIRE_FALSE(writeRes);
    CHECK(writeRes.error().code == Error::Code::NotSupported);

    auto const readRes = backend.property(unknown);
    REQUIRE_FALSE(readRes);
    CHECK(readRes.error().code == Error::Code::NotSupported);

    CHECK(backend.queryProperty(unknown) == PropertyInfo{});
  }
} // namespace ao::audio::test
