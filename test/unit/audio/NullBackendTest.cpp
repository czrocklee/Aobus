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

#include <cstdint>
#include <type_traits>

namespace ao::audio::test
{
  static_assert(std::is_nothrow_default_constructible_v<NullBackend>);

  TEST_CASE("NullBackend - lifecycle is inert and identifies the shared null route", "[audio][unit][null-backend]")
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

  TEST_CASE("NullBackend - retains identified application slots without claiming a physical endpoint",
            "[audio][unit][null-backend]")
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

  TEST_CASE("NullBackend - discarding stereo reuse does not make unidentified PCM a wildcard",
            "[audio][unit][null-backend]")
  {
    auto backend = NullBackend{};
    auto unidentified = PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le};
    auto identified = unidentified;
    identified.speakerMask = 0x3;
    CHECK_FALSE(isSamePcmMode(unidentified, identified));
    CHECK_FALSE(backend.Backend::canReusePcmMode(unidentified, identified));
    CHECK(backend.canReusePcmMode(unidentified, identified));
    CHECK(backend.canReusePcmMode(identified, unidentified));
    CHECK(backend.canReusePcmMode(identified, identified));

    SECTION("different rates remain different storage")
    {
      identified.sampleRate = 44100;
    }

    SECTION("different counts remain different storage")
    {
      identified.channels = 1;
    }

    SECTION("different encodings remain different storage")
    {
      identified.encoding = SampleEncoding::Signed32Le;
    }

    SECTION("alternate identified stereo is not FL FR")
    {
      identified.speakerMask = 0x30;
    }

    SECTION("a partial role set is not canonical stereo")
    {
      identified.speakerMask = 0x1;
    }

    SECTION("zero rates cannot establish reusable storage")
    {
      unidentified.sampleRate = 0;
      identified.sampleRate = 0;
    }

    SECTION("unknown encoding cannot establish reusable storage")
    {
      unidentified.encoding = SampleEncoding::Unknown;
      identified.encoding = SampleEncoding::Unknown;
    }

    CHECK_FALSE(backend.canReusePcmMode(unidentified, identified));
    CHECK_FALSE(backend.canReusePcmMode(identified, unidentified));
  }

  TEST_CASE("NullBackend - source without a lossless PCM encoding is rejected", "[audio][unit][null-backend]")
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

  TEST_CASE("NullBackend - zero-precision source stays refused without a prewarm hint", "[audio][unit][null-backend]")
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

  TEST_CASE("NullBackend - zero-geometry source is rejected as invalid input", "[audio][unit][null-backend]")
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

  TEST_CASE("NullBackend - volume and mute properties remain readable and writable", "[audio][unit][null-backend]")
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

  TEST_CASE("NullBackend - unknown properties are unsupported", "[audio][unit][null-backend]")
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
