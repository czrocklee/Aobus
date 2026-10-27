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
    constexpr std::uint32_t kRate = 48000;
    // WAVE-style FL | FR, the canonical two-speaker identity at this boundary.
    constexpr std::uint32_t kFrontStereoMask = 0x3;
    // WAVE-style FL | FC, a different complete two-speaker identity.
    constexpr std::uint32_t kFrontLeftCenterMask = 0x5;

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

    SignalFormat maskedSignal(std::uint32_t const speakerMask)
    {
      return SignalFormat{.sampleRate = kRate, .channels = 2, .precisionBits = 16, .speakerMask = speakerMask};
    }

    OpenedPcmMode maskedMode(std::uint32_t const clientMask, std::uint32_t const endpointMask)
    {
      auto clientFormat = PcmFormat{.sampleRate = kRate, .channels = 2, .encoding = SampleEncoding::Signed16Le};
      clientFormat.speakerMask = clientMask;

      return OpenedPcmMode{
        .clientFormat = clientFormat, .optEndpoint = ConfirmedEndpoint{.signalFormat = maskedSignal(endpointMask)}};
    }
  } // namespace

  TEST_CASE("OpenedModeValidation - stereo identity aliases remain distinct canonical PCM modes",
            "[audio][unit][engine]")
  {
    auto const firstMask = GENERATE(0x0U, 0x3U);
    auto const current =
      PcmFormat{.sampleRate = kRate, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = firstMask};
    auto next = current;
    next.speakerMask = firstMask == 0U ? 0x3U : 0U;

    CHECK(isStereoIdentityAlias(current, next));
    CHECK(isStereoIdentityAlias(next, current));
    CHECK_FALSE(isSamePcmMode(current, next));
    // A reuse candidate is not permission for open() or the converter to
    // substitute another identity for this source.
    auto const invalidRes = validateOpenedMode(signalFormat(next), OpenedPcmMode{.clientFormat = current});
    REQUIRE_FALSE(invalidRes);
    CHECK(invalidRes.error().message == "Backend returned a different client speaker mask");
  }

  TEST_CASE("OpenedModeValidation - stereo identity aliases exclude geometry encoding and other masks",
            "[audio][unit][engine]")
  {
    auto current = PcmFormat{.sampleRate = kRate, .channels = 2, .encoding = SampleEncoding::Signed16Le};
    auto next = current;
    next.speakerMask = 0x3;

    SECTION("zero rate")
    {
      current.sampleRate = 0;
      next.sampleRate = 0;
    }

    SECTION("different rate")
    {
      next.sampleRate = 44100;
    }

    SECTION("mono is not stereo")
    {
      current.channels = 1;
      next.channels = 1;
    }

    SECTION("multichannel is not stereo")
    {
      current.channels = 4;
      next.channels = 4;
    }

    SECTION("different channel counts")
    {
      next.channels = 1;
    }

    SECTION("different encoding")
    {
      next.encoding = SampleEncoding::Signed32Le;
    }

    SECTION("unknown encoding")
    {
      current.encoding = SampleEncoding::Unknown;
      next.encoding = SampleEncoding::Unknown;
    }

    SECTION("an alternate two-speaker set is not FL FR")
    {
      next.speakerMask = kFrontLeftCenterMask;
    }

    SECTION("equal unidentified masks need no alias")
    {
      next.speakerMask = 0;
    }

    SECTION("equal identified masks need no alias")
    {
      current.speakerMask = 0x3;
    }

    CHECK_FALSE(isStereoIdentityAlias(current, next));
    CHECK_FALSE(isStereoIdentityAlias(next, current));
  }

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

    auto const [encoding, endpointBits] =
      GENERATE(std::pair<SampleEncoding, std::uint8_t>{SampleEncoding::Signed16Le, 16},
               std::pair<SampleEncoding, std::uint8_t>{SampleEncoding::Signed32Le, 32});
    auto const floatToInteger = openedMode(encoding, endpointBits);
    auto const validatedRes = validateOpenedMode(floatSignal, floatToInteger);

    REQUIRE_FALSE(validatedRes);
    CHECK(validatedRes.error().code == Error::Code::FormatRejected);
    CHECK(validatedRes.error().message.contains(
      encoding == SampleEncoding::Signed16Le ? "Backend returned lossy S16_LE" : "Backend returned lossy S32_LE"));
  }

  TEST_CASE("validateOpenedMode - a client mode that loses or substitutes the source speaker mask is rejected",
            "[audio][unit][engine]")
  {
    auto mode = maskedMode(kFrontStereoMask, kFrontStereoMask);

    SECTION("substituted same-count mask")
    {
      mode.clientFormat.speakerMask = kFrontLeftCenterMask;
    }

    SECTION("lost mask")
    {
      mode.clientFormat.speakerMask = 0U;
    }

    auto const validatedRes = validateOpenedMode(maskedSignal(kFrontStereoMask), mode);

    REQUIRE_FALSE(validatedRes);
    CHECK(validatedRes.error().code == Error::Code::FormatRejected);
    CHECK(validatedRes.error().message == "Backend returned a different client speaker mask");
  }

  TEST_CASE("validateOpenedMode - a known contradicting endpoint speaker mask is rejected", "[audio][unit][engine]")
  {
    auto const validatedRes =
      validateOpenedMode(maskedSignal(kFrontStereoMask), maskedMode(kFrontStereoMask, kFrontLeftCenterMask));

    REQUIRE_FALSE(validatedRes);
    CHECK(validatedRes.error().code == Error::Code::FormatRejected);
    CHECK(validatedRes.error().message == "Backend confirmed an endpoint with a different speaker mask");
  }

  TEST_CASE("validateOpenedMode - an unknown mask neither rejects nor inherits the identified speaker layout",
            "[audio][unit][engine]")
  {
    SECTION("endpoint mask stays zero for an identified source layout")
    {
      auto const mode = maskedMode(kFrontStereoMask, 0U);

      CHECK(validateOpenedMode(maskedSignal(kFrontStereoMask), mode));
      // Unknown endpoint positions provide no speaker proof and are never
      // filled from the request.
      CHECK(mode.optEndpoint->signalFormat.speakerMask == 0U);
    }

    SECTION("an unassigned source cannot contradict an identified endpoint")
    {
      CHECK(validateOpenedMode(maskedSignal(0U), maskedMode(0U, kFrontStereoMask)));
    }
  }

  TEST_CASE("validateOpenedMode - matching identified speaker masks on client and endpoint are accepted",
            "[audio][unit][engine]")
  {
    CHECK(validateOpenedMode(maskedSignal(kFrontStereoMask), maskedMode(kFrontStereoMask, kFrontStereoMask)));
  }
} // namespace ao::audio::detail::test
