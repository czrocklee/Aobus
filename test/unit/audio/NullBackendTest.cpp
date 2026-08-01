// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/audio/NullBackend.h>

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
    auto backend = NullBackend{};
    auto const sourceFormat = SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 16};

    auto const optHint = backend.prewarmFormatHint(sourceFormat);
    REQUIRE(optHint);
    CHECK(*optHint == pcmFormat(sourceFormat, SampleEncoding::Signed16Le));

    auto const openResult = backend.open(sourceFormat, nullptr);
    REQUIRE(openResult);
    CHECK(openResult->clientFormat == pcmFormat(sourceFormat, SampleEncoding::Signed16Le));
    // A discarding sink has no direct endpoint evidence, but its client mode
    // still preserves the source precision.
    CHECK_FALSE(openResult->optEndpoint);
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
    auto backend = NullBackend{};
    auto const opened = backend.open(SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 33}, nullptr);

    CHECK_FALSE(backend.prewarmFormatHint(SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 33}));
    REQUIRE_FALSE(opened);
    CHECK(opened.error().code == Error::Code::NotSupported);
  }

  TEST_CASE("NullBackend - volume and mute properties remain readable and writable", "[audio][unit][null-backend]")
  {
    auto backend = NullBackend{};

    auto volume = backend.property(PropertyId::Volume);
    REQUIRE(volume);
    CHECK(std::get<float>(*volume) == 1.0F);

    auto muted = backend.property(PropertyId::Muted);
    REQUIRE(muted);
    CHECK_FALSE(std::get<bool>(*muted));

    REQUIRE(backend.setProperty(PropertyId::Volume, PropertyValue{0.25F}));
    REQUIRE(backend.setProperty(PropertyId::Muted, PropertyValue{true}));

    volume = backend.property(PropertyId::Volume);
    REQUIRE(volume);
    CHECK(std::get<float>(*volume) == 0.25F);

    muted = backend.property(PropertyId::Muted);
    REQUIRE(muted);
    CHECK(std::get<bool>(*muted));

    auto const expectedInfo =
      PropertyInfo{.canRead = true, .canWrite = true, .isAvailable = true, .emitsChangeNotifications = false};
    CHECK(backend.queryProperty(PropertyId::Volume) == expectedInfo);
    CHECK(backend.queryProperty(PropertyId::Muted) == expectedInfo);
  }

  TEST_CASE("NullBackend - unknown properties are unsupported", "[audio][unit][null-backend]")
  {
    auto backend = NullBackend{};
    auto const unknown = static_cast<PropertyId>(UINT8_C(0xff));

    auto const writeResult = backend.setProperty(unknown, PropertyValue{false});
    REQUIRE_FALSE(writeResult);
    CHECK(writeResult.error().code == Error::Code::NotSupported);

    auto const readResult = backend.property(unknown);
    REQUIRE_FALSE(readResult);
    CHECK(readResult.error().code == Error::Code::NotSupported);

    CHECK(backend.queryProperty(unknown) == PropertyInfo{});
  }
} // namespace ao::audio::test
