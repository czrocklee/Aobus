// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/detail/DecoderOutput.h"

#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ao::audio::test
{
  TEST_CASE("losslessPcmEncodings - integer candidates preserve source precision", "[audio][unit][decoder]")
  {
    SECTION("16-bit source prefers its native container")
    {
      auto const encodings = detail::losslessPcmEncodings(SignalFormat{.precisionBits = 16});

      CHECK(encodings == std::vector{SampleEncoding::Signed16Le,
                                     SampleEncoding::Signed24PackedLe,
                                     SampleEncoding::Signed24In32Le,
                                     SampleEncoding::Signed32Le,
                                     SampleEncoding::Float32Le});
    }

    SECTION("24-bit source never advertises S16")
    {
      auto const encodings = detail::losslessPcmEncodings(SignalFormat{.precisionBits = 24});

      CHECK(encodings == std::vector{SampleEncoding::Signed24PackedLe,
                                     SampleEncoding::Signed24In32Le,
                                     SampleEncoding::Signed32Le,
                                     SampleEncoding::Float32Le});
    }

    SECTION("32-bit source only advertises S32")
    {
      auto const encodings = detail::losslessPcmEncodings(SignalFormat{.precisionBits = 32});

      CHECK(encodings == std::vector{SampleEncoding::Signed32Le});
    }
  }

  TEST_CASE("losslessPcmEncodings - float source cannot be converted to integer", "[audio][unit][decoder]")
  {
    auto const encodings =
      detail::losslessPcmEncodings(SignalFormat{.precisionBits = 32, .sampleKind = SampleKind::FloatingPoint});

    CHECK(encodings == std::vector{SampleEncoding::Float32Le});
  }

  TEST_CASE("losslessPcmEncodings - unsupported precision has no output candidate", "[audio][unit][decoder]")
  {
    CHECK(detail::losslessPcmEncodings(SignalFormat{.precisionBits = 0}).empty());
    CHECK(detail::losslessPcmEncodings(SignalFormat{.precisionBits = 33}).empty());
    CHECK(
      detail::losslessPcmEncodings(SignalFormat{.precisionBits = 24, .sampleKind = SampleKind::FloatingPoint}).empty());
  }

  TEST_CASE("preferredLosslessPcmEncoding - returns the first lossless candidate", "[audio][unit][decoder]")
  {
    CHECK(detail::preferredLosslessPcmEncoding(SignalFormat{.precisionBits = 16}) == SampleEncoding::Signed16Le);
    CHECK(detail::preferredLosslessPcmEncoding(SignalFormat{.precisionBits = 24}) == SampleEncoding::Signed24PackedLe);
    CHECK(detail::preferredLosslessPcmEncoding(SignalFormat{.precisionBits = 32}) == SampleEncoding::Signed32Le);
    CHECK(detail::preferredLosslessPcmEncoding(
            SignalFormat{.precisionBits = 32, .sampleKind = SampleKind::FloatingPoint}) == SampleEncoding::Float32Le);
    CHECK_FALSE(detail::preferredLosslessPcmEncoding(SignalFormat{.precisionBits = 33}));
  }
} // namespace ao::audio::test
