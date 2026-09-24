// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/detail/OpenedModeValidation.h"

#include <ao/Error.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>
#include <string_view>
#include <utility>

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

  TEST_CASE("validateOpenedMode - a lossy result without a confirmed endpoint is rejected", "[audio][unit][engine]")
  {
    auto const validatedRes = validateOpenedMode(integerSignal(24), unconfirmedMode(SampleEncoding::Signed16Le));

    REQUIRE_FALSE(validatedRes);
    CHECK(validatedRes.error().code == Error::Code::FormatRejected);
    CHECK(validatedRes.error().message.contains("Backend returned lossy S16_LE"));
  }

  TEST_CASE("validateOpenedMode - a lossless result without a confirmed endpoint is accepted", "[audio][unit][engine]")
  {
    CHECK(validateOpenedMode(integerSignal(24), unconfirmedMode(SampleEncoding::Signed24PackedLe)));
    CHECK(validateOpenedMode(integerSignal(16), unconfirmedMode(SampleEncoding::Signed32Le)));
  }

  TEST_CASE("validateOpenedMode - a confirmed endpoint does not authorize a lossy client format",
            "[audio][unit][engine]")
  {
    auto const mode = openedMode(SampleEncoding::Signed16Le, 16);

    auto const validatedRes = validateOpenedMode(integerSignal(24), mode);

    REQUIRE_FALSE(validatedRes);
    CHECK(validatedRes.error().code == Error::Code::FormatRejected);
    CHECK(validatedRes.error().message.contains("Backend returned lossy S16_LE"));
  }

  TEST_CASE("validateOpenedMode - a confirmed endpoint at source precision loses nothing", "[audio][unit][engine]")
  {
    // 24-bit content in a 32-bit container feeding a 24-bit converter. The
    // endpoint is narrower than the container but not narrower than the signal,
    // so this must be admitted and must not be reported as a reduction.
    auto const mode = openedMode(SampleEncoding::Signed32Le, 24);

    CHECK(validateOpenedMode(integerSignal(24), mode));
  }

  TEST_CASE("validateOpenedMode - a confirmed endpoint narrower than the source is rejected", "[audio][unit][engine]")
  {
    auto const validatedRes = validateOpenedMode(integerSignal(32), openedMode(SampleEncoding::Signed32Le, 24));

    REQUIRE_FALSE(validatedRes);
    CHECK(validatedRes.error().code == Error::Code::FormatRejected);
    CHECK(validatedRes.error().message.contains("24-bit endpoint for a 32-bit source"));
  }

  TEST_CASE("validateOpenedMode - an endpoint wider than its client encoding is rejected", "[audio][unit][engine]")
  {
    auto const mode = openedMode(SampleEncoding::Signed16Le, 24);

    auto const validatedRes = validateOpenedMode(integerSignal(16), mode);

    REQUIRE_FALSE(validatedRes);
    CHECK(validatedRes.error().code == Error::Code::FormatRejected);
    CHECK(validatedRes.error().message.contains("cannot carry it"));
  }

  TEST_CASE("validateOpenedMode - a zero-precision endpoint is rejected", "[audio][unit][engine]")
  {
    REQUIRE_FALSE(validateOpenedMode(integerSignal(24), openedMode(SampleEncoding::Signed24PackedLe, 0)));
  }

  TEST_CASE("validateOpenedMode - substituted rate or channel count is rejected", "[audio][unit][engine]")
  {
    auto mode = unconfirmedMode(SampleEncoding::Signed24PackedLe);
    auto expectedMessage = std::string_view{};

    SECTION("substituted client sample rate")
    {
      mode.clientFormat.sampleRate = 44100;
      expectedMessage = "Backend returned 44100 Hz / 2 ch for a 48000 Hz / 2 ch source";
    }

    SECTION("substituted client channel count")
    {
      mode.clientFormat.channels = 1;
      expectedMessage = "Backend returned 48000 Hz / 1 ch for a 48000 Hz / 2 ch source";
    }

    auto const validatedRes = validateOpenedMode(integerSignal(24), mode);

    REQUIRE_FALSE(validatedRes);
    CHECK(validatedRes.error().code == Error::Code::FormatRejected);
    CHECK(validatedRes.error().message == expectedMessage);
  }

  TEST_CASE("validateOpenedMode - an endpoint with a different rate or layout is rejected", "[audio][unit][engine]")
  {
    auto mode = openedMode(SampleEncoding::Signed16Le, 16);

    SECTION("substituted endpoint sample rate")
    {
      mode.optEndpoint->signalFormat.sampleRate = 44100;
    }

    SECTION("substituted endpoint channel count")
    {
      mode.optEndpoint->signalFormat.channels = 1;
    }

    auto const validatedRes = validateOpenedMode(integerSignal(16), mode);

    REQUIRE_FALSE(validatedRes);
    CHECK(validatedRes.error().code == Error::Code::FormatRejected);
    CHECK(validatedRes.error().message == "Backend confirmed an endpoint with a different rate or layout");
  }

  TEST_CASE("validateOpenedMode - an endpoint in a different sample domain is rejected", "[audio][unit][engine]")
  {
    auto mode = openedMode(SampleEncoding::Signed16Le, 16);
    mode.optEndpoint->signalFormat.sampleKind = SampleKind::FloatingPoint;

    auto const validatedRes = validateOpenedMode(integerSignal(16), mode);

    REQUIRE_FALSE(validatedRes);
    CHECK(validatedRes.error().message.contains("sample domain"));
  }

  TEST_CASE("validateOpenedMode - float signals are never quantized to integers", "[audio][unit][engine]")
  {
    auto const floatSignal =
      SignalFormat{.sampleRate = kRate, .channels = 2, .precisionBits = 32, .sampleKind = SampleKind::FloatingPoint};

    auto const [encoding, endpointBits] = GENERATE(
      std::pair{SampleEncoding::Signed16Le, std::uint8_t{16}}, std::pair{SampleEncoding::Signed32Le, std::uint8_t{32}});
    auto const floatToInteger = openedMode(encoding, endpointBits);
    auto const validatedRes = validateOpenedMode(floatSignal, floatToInteger);

    REQUIRE_FALSE(validatedRes);
    CHECK(validatedRes.error().code == Error::Code::FormatRejected);
    CHECK(validatedRes.error().message.contains(
      encoding == SampleEncoding::Signed16Le ? "Backend returned lossy S16_LE" : "Backend returned lossy S32_LE"));
  }
} // namespace ao::audio::detail::test
