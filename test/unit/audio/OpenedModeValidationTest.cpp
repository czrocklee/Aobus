// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/detail/OpenedModeValidation.h"

#include <ao/Error.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace ao::audio::detail::test
{
  namespace
  {
    constexpr auto kRate = std::uint32_t{48000};

    SignalFormat integerSignal(std::uint8_t const precisionBits)
    {
      return SignalFormat{.sampleRate = kRate, .channels = 2, .precisionBits = precisionBits};
    }

    OpenedPcmMode openedMode(SampleEncoding const encoding, std::uint8_t const endpointBits)
    {
      return OpenedPcmMode{.clientFormat = PcmFormat{.sampleRate = kRate, .channels = 2, .encoding = encoding},
                           .optEndpoint = ConfirmedEndpoint{.signalFormat = integerSignal(endpointBits)}};
    }

    OpenedPcmMode unconfirmedMode(SampleEncoding const encoding)
    {
      return OpenedPcmMode{.clientFormat = PcmFormat{.sampleRate = kRate, .channels = 2, .encoding = encoding}};
    }
  } // namespace

  TEST_CASE("validateOpenedMode - a lossy result without a confirmed endpoint is rejected",
            "[audio][regression][engine]")
  {
    auto const validated = validateOpenedMode(integerSignal(24), unconfirmedMode(SampleEncoding::Signed16Le));

    REQUIRE_FALSE(validated);
    CHECK(validated.error().code == Error::Code::FormatRejected);
    CHECK(validated.error().message.contains("Backend returned lossy S16_LE"));
  }

  TEST_CASE("validateOpenedMode - a lossless result without a confirmed endpoint is accepted", "[audio][unit][engine]")
  {
    CHECK(validateOpenedMode(integerSignal(24), unconfirmedMode(SampleEncoding::Signed24PackedLe)));
    CHECK(validateOpenedMode(integerSignal(16), unconfirmedMode(SampleEncoding::Signed32Le)));
  }

  TEST_CASE("validateOpenedMode - a confirmed endpoint does not authorize a lossy client format",
            "[audio][regression][engine]")
  {
    auto const mode = openedMode(SampleEncoding::Signed16Le, 16);

    auto const validated = validateOpenedMode(integerSignal(24), mode);

    REQUIRE_FALSE(validated);
    CHECK(validated.error().code == Error::Code::FormatRejected);
    CHECK(validated.error().message.contains("Backend returned lossy S16_LE"));
  }

  TEST_CASE("validateOpenedMode - a confirmed endpoint at source precision loses nothing",
            "[audio][regression][engine]")
  {
    // 24-bit content in a 32-bit container feeding a 24-bit converter. The
    // endpoint is narrower than the container but not narrower than the signal,
    // so this must be admitted and must not be reported as a reduction.
    auto const mode = openedMode(SampleEncoding::Signed32Le, 24);

    CHECK(validateOpenedMode(integerSignal(24), mode));
  }

  TEST_CASE("validateOpenedMode - a confirmed endpoint narrower than the source is rejected",
            "[audio][regression][engine]")
  {
    auto const validated = validateOpenedMode(integerSignal(32), openedMode(SampleEncoding::Signed32Le, 24));

    REQUIRE_FALSE(validated);
    CHECK(validated.error().code == Error::Code::FormatRejected);
    CHECK(validated.error().message.contains("24-bit endpoint for a 32-bit source"));
  }

  TEST_CASE("validateOpenedMode - an endpoint wider than its client encoding is rejected",
            "[audio][regression][engine]")
  {
    auto const mode = openedMode(SampleEncoding::Signed16Le, 24);

    auto const validated = validateOpenedMode(integerSignal(16), mode);

    REQUIRE_FALSE(validated);
    CHECK(validated.error().code == Error::Code::FormatRejected);
    CHECK(validated.error().message.contains("cannot carry it"));
  }

  TEST_CASE("validateOpenedMode - a zero-precision endpoint is rejected", "[audio][unit][engine]")
  {
    REQUIRE_FALSE(validateOpenedMode(integerSignal(24), openedMode(SampleEncoding::Signed24PackedLe, 0)));
  }

  TEST_CASE("validateOpenedMode - substituted rate or channel count is rejected", "[audio][regression][engine]")
  {
    auto rateChanged = unconfirmedMode(SampleEncoding::Signed24PackedLe);
    rateChanged.clientFormat.sampleRate = 44100;

    auto const validatedRate = validateOpenedMode(integerSignal(24), rateChanged);
    REQUIRE_FALSE(validatedRate);
    CHECK(validatedRate.error().code == Error::Code::FormatRejected);

    auto channelsChanged = unconfirmedMode(SampleEncoding::Signed24PackedLe);
    channelsChanged.clientFormat.channels = 1;

    REQUIRE_FALSE(validateOpenedMode(integerSignal(24), channelsChanged));
  }

  TEST_CASE("validateOpenedMode - an endpoint with a different layout is rejected", "[audio][unit][engine]")
  {
    auto mode = openedMode(SampleEncoding::Signed16Le, 16);
    mode.optEndpoint->signalFormat.sampleRate = 44100;

    REQUIRE_FALSE(validateOpenedMode(integerSignal(16), mode));
  }

  TEST_CASE("validateOpenedMode - an endpoint in a different sample domain is rejected", "[audio][unit][engine]")
  {
    auto mode = openedMode(SampleEncoding::Signed16Le, 16);
    mode.optEndpoint->signalFormat.sampleKind = SampleKind::FloatingPoint;

    auto const validated = validateOpenedMode(integerSignal(16), mode);

    REQUIRE_FALSE(validated);
    CHECK(validated.error().message.contains("sample domain"));
  }

  TEST_CASE("validateOpenedMode - float signals are never quantized to integers", "[audio][regression][engine]")
  {
    auto const floatSignal =
      SignalFormat{.sampleRate = kRate, .channels = 2, .precisionBits = 32, .sampleKind = SampleKind::FloatingPoint};

    auto floatToInteger = openedMode(SampleEncoding::Signed16Le, 16);
    auto const validated = validateOpenedMode(floatSignal, floatToInteger);

    REQUIRE_FALSE(validated);
    CHECK(validated.error().code == Error::Code::FormatRejected);
  }
} // namespace ao::audio::detail::test
