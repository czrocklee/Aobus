// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/NullBackend.h"

#include "BackendTestSupport.h"
#include <ao/Error.h>
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

  TEST_CASE("NullBackend - source without a lossless PCM encoding is rejected", "[audio][unit][null-backend]")
  {
    auto target = NoopRenderTarget{};
    auto backend = NullBackend{};
    auto const openedRes = backend.open(SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 33}, target);

    CHECK_FALSE(backend.prewarmFormatHint(SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 33}));
    REQUIRE_FALSE(openedRes);
    CHECK(openedRes.error().code == Error::Code::NotSupported);
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
