// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/audio/backend/AlsaExclusiveBackend.h>

#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_test_macros.hpp>

#include <tuple>

namespace ao::audio::backend::test
{
  TEST_CASE("AlsaExclusiveBackend - non-hardware PCM is rejected", "[audio][regression][alsa]")
  {
    auto const device = Device{
      .id = DeviceId{"null"}, .displayName = "ALSA null plugin", .description = "null", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive};

    auto const opened = backend.open(SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 24}, nullptr);

    REQUIRE_FALSE(opened);
    CHECK(opened.error().code == Error::Code::FormatRejected);
    CHECK(opened.error().message.contains("direct hardware PCM"));
  }

  TEST_CASE("AlsaExclusiveBackend - unsupported signal is rejected before native device inspection",
            "[audio][regression][alsa]")
  {
    auto const device = Device{
      .id = DeviceId{"null"}, .displayName = "ALSA null plugin", .description = "null", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive};

    auto const opened = backend.open(SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 33}, nullptr);

    REQUIRE_FALSE(opened);
    CHECK(opened.error().code == Error::Code::NotSupported);
    CHECK(opened.error().message == "No lossless PCM encoding is available for ALSA");
  }

  TEST_CASE("AlsaExclusiveBackend - a missing device is reported as such, never as a format problem",
            "[audio][regression][alsa]")
  {
    // A failed native open must surface its own cause. Translating it into a
    // format decision is exactly how an unavailable device once turned into a
    // silent 16-bit fallback.
    auto const device = Device{
      .id = DeviceId{"hw:127,0"}, .displayName = "Absent card", .description = "hw:127,0", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive};

    auto const opened = backend.open(SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 24}, nullptr);

    REQUIRE_FALSE(opened);
    CHECK(opened.error().code != Error::Code::FormatRejected);
    CHECK(opened.error().message.contains("Failed to open ALSA device"));
  }

  TEST_CASE("AlsaExclusiveBackend - a hint is never treated as an opened mode", "[audio][regression][alsa]")
  {
    // Nothing is cached before a successful open, so a device that was never
    // opened predicts only from immutable policy.
    auto const device = Device{
      .id = DeviceId{"hw:127,0"}, .displayName = "Absent card", .description = "hw:127,0", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive};
    auto const sourceFormat = SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 24};

    std::ignore = backend.open(sourceFormat, nullptr);

    auto const optHint = backend.prewarmFormatHint(sourceFormat);

    REQUIRE(optHint);
    CHECK(optHint->encoding == SampleEncoding::Signed24PackedLe);
  }
} // namespace ao::audio::backend::test
