// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/AlsaModeSelector.h"

#include <ao/Error.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

namespace ao::audio::backend::detail::test
{
  namespace
  {
    SignalFormat integerSignal(std::uint8_t const precisionBits)
    {
      return SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = precisionBits};
    }
  } // namespace

  TEST_CASE("selectAlsaMode - a 24-bit capable device keeps the source precision", "[audio][unit][alsa]")
  {
    auto const evidence =
      std::array{AlsaModeEvidence{.encoding = SampleEncoding::Signed16Le, .optSignificantBits = 16},
                 AlsaModeEvidence{.encoding = SampleEncoding::Signed24PackedLe, .optSignificantBits = 24},
                 AlsaModeEvidence{.encoding = SampleEncoding::Signed32Le, .optSignificantBits = 32}};

    auto const selected = selectAlsaMode(integerSignal(24), evidence);

    REQUIRE(selected);
    CHECK(selected->encoding == SampleEncoding::Signed24PackedLe);
    CHECK(selected->endpointPrecisionBits == 24);
  }

  TEST_CASE("selectAlsaMode - a 16-bit endpoint rejects a 24-bit signal", "[audio][regression][alsa]")
  {
    auto const evidence =
      std::array{AlsaModeEvidence{.encoding = SampleEncoding::Signed16Le, .optSignificantBits = 16}};

    auto const selected = selectAlsaMode(integerSignal(24), evidence);

    REQUIRE_FALSE(selected);
    CHECK(selected.error().code == Error::Code::FormatRejected);
    CHECK(selected.error().message.contains("no confirmed lossless endpoint"));
    CHECK(selected.error().message.contains("S16_LE sbits=16"));
  }

  TEST_CASE("selectAlsaMode - documented lossless order chooses the native container first", "[audio][unit][alsa]")
  {
    auto const evidence =
      std::array{AlsaModeEvidence{.encoding = SampleEncoding::Signed24In32Le, .optSignificantBits = 24},
                 AlsaModeEvidence{.encoding = SampleEncoding::Signed24PackedLe, .optSignificantBits = 24}};

    auto const selected = selectAlsaMode(integerSignal(24), evidence);

    REQUIRE(selected);
    CHECK(selected->encoding == SampleEncoding::Signed24PackedLe);
    CHECK(selected->endpointPrecisionBits == 24);
  }

  TEST_CASE("selectAlsaMode - significant bits decide whether a wide container is lossless",
            "[audio][regression][alsa]")
  {
    auto const evidence =
      std::array{AlsaModeEvidence{.encoding = SampleEncoding::Signed32Le, .optSignificantBits = 24}};

    auto const lossless = selectAlsaMode(integerSignal(24), evidence);

    REQUIRE(lossless);
    CHECK(lossless->encoding == SampleEncoding::Signed32Le);
    CHECK(lossless->endpointPrecisionBits == 24);

    auto const reduced = selectAlsaMode(integerSignal(32), evidence);

    REQUIRE_FALSE(reduced);
    CHECK(reduced.error().code == Error::Code::FormatRejected);
  }

  TEST_CASE("selectAlsaMode - missing significant-bit evidence is not treated as full precision",
            "[audio][regression][alsa]")
  {
    auto const evidence = std::array{AlsaModeEvidence{.encoding = SampleEncoding::Signed32Le}};

    auto const selected = selectAlsaMode(integerSignal(32), evidence);

    REQUIRE_FALSE(selected);
    CHECK(selected.error().code == Error::Code::FormatRejected);
    CHECK(selected.error().message.contains("S32_LE sbits=unknown"));
  }

  TEST_CASE("selectAlsaMode - endpoint precision never exceeds its encoding", "[audio][regression][alsa]")
  {
    auto const evidence =
      std::array{AlsaModeEvidence{.encoding = SampleEncoding::Signed16Le, .optSignificantBits = 32}};

    auto const selected = selectAlsaMode(integerSignal(24), evidence);

    REQUIRE_FALSE(selected);
    CHECK(selected.error().code == Error::Code::FormatRejected);
  }

  TEST_CASE("selectAlsaMode - an integer signal is not quantized into float", "[audio][regression][alsa]")
  {
    auto const evidence = std::array{AlsaModeEvidence{.encoding = SampleEncoding::Float32Le, .optSignificantBits = 32}};

    auto const selected = selectAlsaMode(integerSignal(32), evidence);

    REQUIRE_FALSE(selected);
    CHECK(selected.error().code == Error::Code::FormatRejected);
  }

  TEST_CASE("selectAlsaMode - a float signal is not quantized into integer", "[audio][regression][alsa]")
  {
    auto const evidence =
      std::array{AlsaModeEvidence{.encoding = SampleEncoding::Signed32Le, .optSignificantBits = 32}};
    auto const floatSignal =
      SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 32, .sampleKind = SampleKind::FloatingPoint};

    auto const selected = selectAlsaMode(floatSignal, evidence);

    REQUIRE_FALSE(selected);
    CHECK(selected.error().code == Error::Code::FormatRejected);
  }

  TEST_CASE("selectAlsaMode - empty evidence reports what was inspected", "[audio][unit][alsa]")
  {
    auto const selected = selectAlsaMode(integerSignal(24), {});

    REQUIRE_FALSE(selected);
    CHECK(selected.error().message.contains("none"));
  }

  TEST_CASE("selectAlsaMode - a signal without precision is rejected", "[audio][unit][alsa]")
  {
    auto const evidence =
      std::array{AlsaModeEvidence{.encoding = SampleEncoding::Signed16Le, .optSignificantBits = 16}};

    auto const selected = selectAlsaMode(integerSignal(0), evidence);

    REQUIRE_FALSE(selected);
    CHECK(selected.error().code == Error::Code::InvalidInput);
  }
} // namespace ao::audio::backend::detail::test
