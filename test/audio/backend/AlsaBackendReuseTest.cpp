// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/AlsaExclusiveBackend.h"
#include "test/audio/BackendTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::audio::backend::test
{
  TEST_CASE("AlsaExclusiveBackend - unopened reuse keeps exact equality but refuses stereo aliases",
            "[audio][unit][alsa]")
  {
    auto const device = Device{
      .id = DeviceId{"hw:127,0"}, .displayName = "Absent card", .description = "hw:127,0", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive};
    Backend const& dispatch = backend;
    auto const unidentified = PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le};
    auto identified = unidentified;
    identified.speakerMask = 0x3;

    CHECK(backend.canReuseFormat(identified, identified));
    // Equality does not validate a mode or require an installed stream.
    CHECK(backend.canReuseFormat(PcmFormat{}, PcmFormat{}));
    CHECK_FALSE(backend.canReuseFormat(unidentified, identified));
    CHECK_FALSE(backend.canReuseFormat(identified, unidentified));
    CHECK_FALSE(dispatch.canReuseFormat(unidentified, identified));
    CHECK_FALSE(dispatch.canReuseFormat(identified, unidentified));
  }

  TEST_CASE("AlsaExclusiveBackend - failed native open and close cannot authorize stereo alias reuse",
            "[audio][unit][alsa]")
  {
    auto const device = Device{
      .id = DeviceId{"hw:127,0"}, .displayName = "Absent card", .description = "hw:127,0", .backendId = kBackendAlsa};
    auto target = ::ao::audio::test::NoopRenderTarget{};
    auto backend = AlsaExclusiveBackend{device, kProfileExclusive};
    auto const unidentified = PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le};
    auto identified = unidentified;
    identified.speakerMask = 0x3;

    // Reach the real native open failure, not an admission guard or injected state.
    auto const openedRes =
      backend.open(SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 0x3}, target);

    REQUIRE_FALSE(openedRes);
    CHECK(openedRes.error().error.code != Error::Code::FormatRejected);
    CHECK(openedRes.error().error.message.contains("Failed to open ALSA device 'hw:127,0'"));
    CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    CHECK_FALSE(backend.canReuseFormat(unidentified, identified));
    CHECK_FALSE(backend.canReuseFormat(identified, unidentified));
    CHECK(backend.canReuseFormat(identified, identified));

    backend.close();

    CHECK_FALSE(backend.canReuseFormat(unidentified, identified));
    CHECK_FALSE(backend.canReuseFormat(identified, unidentified));
    CHECK(backend.canReuseFormat(identified, identified));
  }
} // namespace ao::audio::backend::test
