// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "lib/audio/PcmConversion.h"

#include <ao/Error.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ao::audio::test
{
  TEST_CASE("PcmConversion - interleaves channels and pads frames")
  {
    SECTION("Stereo 16-bit to 32-bit interleaved")
    {
      auto const left = std::to_array<std::int16_t>({0x1111, 0x2222});
      auto const right = std::to_array<std::int16_t>({0x3333, 0x4444});

      auto const channels = std::array<std::span<std::int16_t const>, 2>{left, right};
      auto destination = std::array<std::int32_t, 4>{};

      interleaveAndPadPcmSamples<std::int16_t, std::int32_t>(channels, destination, 16);

      CHECK(destination[0] == 0x11110000); // L0
      CHECK(destination[1] == 0x33330000); // R0
      CHECK(destination[2] == 0x22220000); // L1
      CHECK(destination[3] == 0x44440000); // R1
    }

    SECTION("interleaveAndPad returns immediately for empty channel list")
    {
      auto destination = std::array<std::int32_t, 4>{1, 2, 3, 4};
      interleaveAndPadPcmSamples<std::int16_t, std::int32_t>({}, destination, 16);
      CHECK(destination[0] == 1);
      CHECK(destination[1] == 2);
      CHECK(destination[2] == 3);
      CHECK(destination[3] == 4);
    }

    SECTION("interleaveAndPad truncates to destination frame capacity")
    {
      auto const left = std::to_array<std::int16_t>({0x1, 0x2, 0x3});
      auto const right = std::to_array<std::int16_t>({0x4, 0x5, 0x6});
      auto const channels = std::array<std::span<std::int16_t const>, 2>{left, right};
      auto destination = std::array<std::int32_t, 4>{0, 0, 0, 0}; // only 2 frames capacity

      interleaveAndPadPcmSamples<std::int16_t, std::int32_t>(channels, destination, 16);

      CHECK(destination[0] == 0x10000);
      CHECK(destination[1] == 0x40000);
      CHECK(destination[2] == 0x20000);
      CHECK(destination[3] == 0x50000);
    }
  }

  TEST_CASE("convertPcmEncoding - S24-in-32 uses the low 24 bits")
  {
    auto const source = std::to_array<std::byte>(
      {std::byte{0x56}, std::byte{0x34}, std::byte{0x12}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}});
    auto destination = std::vector<std::byte>{};
    auto const sourceFormat =
      PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Signed24PackedLe};

    auto const res =
      convertPcmEncoding(source, sourceFormat, signalFormat(sourceFormat), SampleEncoding::Signed24In32Le, destination);

    REQUIRE(res);
    CHECK(destination == std::vector<std::byte>{std::byte{0x56},
                                                std::byte{0x34},
                                                std::byte{0x12},
                                                std::byte{0x00},
                                                std::byte{0xFF},
                                                std::byte{0xFF},
                                                std::byte{0xFF},
                                                std::byte{0xFF}});
  }

  TEST_CASE("convertPcmEncoding - S16 to S32 uses the full 32-bit scale")
  {
    auto const source = std::to_array<std::byte>({std::byte{0x34}, std::byte{0x12}, std::byte{0xFF}, std::byte{0xFF}});
    auto destination = std::vector<std::byte>{};
    auto const sourceFormat = PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Signed16Le};

    auto const res =
      convertPcmEncoding(source, sourceFormat, signalFormat(sourceFormat), SampleEncoding::Signed32Le, destination);

    REQUIRE(res);
    CHECK(destination == std::vector<std::byte>{std::byte{0x00},
                                                std::byte{0x00},
                                                std::byte{0x34},
                                                std::byte{0x12},
                                                std::byte{0x00},
                                                std::byte{0x00},
                                                std::byte{0xFF},
                                                std::byte{0xFF}});
  }

  TEST_CASE("convertPcmEncoding - S24 to S32 scales to the full 32-bit range")
  {
    auto const source = std::to_array<std::byte>({std::byte{0x56},
                                                  std::byte{0x34},
                                                  std::byte{0x12},
                                                  std::byte{0x00},
                                                  std::byte{0x00},
                                                  std::byte{0x80},
                                                  std::byte{0xFF},
                                                  std::byte{0xFF},
                                                  std::byte{0x7F},
                                                  std::byte{0xFF},
                                                  std::byte{0xFF},
                                                  std::byte{0xFF}});
    auto destination = std::vector<std::byte>{};
    auto const sourceFormat =
      PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Signed24PackedLe};

    auto const res =
      convertPcmEncoding(source, sourceFormat, signalFormat(sourceFormat), SampleEncoding::Signed32Le, destination);

    REQUIRE(res);
    CHECK(destination == std::vector<std::byte>{std::byte{0x00},
                                                std::byte{0x56},
                                                std::byte{0x34},
                                                std::byte{0x12},
                                                std::byte{0x00},
                                                std::byte{0x00},
                                                std::byte{0x00},
                                                std::byte{0x80},
                                                std::byte{0x00},
                                                std::byte{0xFF},
                                                std::byte{0xFF},
                                                std::byte{0x7F},
                                                std::byte{0x00},
                                                std::byte{0xFF},
                                                std::byte{0xFF},
                                                std::byte{0xFF}});
  }

  TEST_CASE("convertPcmEncoding - integer narrowing is rejected")
  {
    // 0x123456 and 0xFFFFFF (-1) packed little-endian.
    auto const source = std::to_array<std::byte>(
      {std::byte{0x56}, std::byte{0x34}, std::byte{0x12}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}});
    auto destination = std::vector<std::byte>{};
    auto const sourceFormat =
      PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Signed24PackedLe};

    auto const res =
      convertPcmEncoding(source, sourceFormat, signalFormat(sourceFormat), SampleEncoding::Signed16Le, destination);

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::NotSupported);
    CHECK(destination.empty());
  }

  TEST_CASE("convertPcmEncoding - float source is never quantized to integer PCM")
  {
    auto const source = std::to_array<std::byte>({std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x3F}});
    auto destination = std::vector<std::byte>{};
    auto const sourceFormat = PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Float32Le};

    auto destinationEncoding = SampleEncoding::Signed24PackedLe;

    SECTION("narrower integer destination")
    {
      destinationEncoding = SampleEncoding::Signed24PackedLe;
    }

    SECTION("equal-width integer destination")
    {
      destinationEncoding = SampleEncoding::Signed32Le;
    }

    auto const res =
      convertPcmEncoding(source, sourceFormat, signalFormat(sourceFormat), destinationEncoding, destination);

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::NotSupported);
    CHECK(destination.empty());
  }

  TEST_CASE("convertPcmEncoding - integer PCM converts to exact Float32 samples")
  {
    auto const source = std::to_array<std::byte>(
      {std::byte{0xFF}, std::byte{0xFF}, std::byte{0x7F}, std::byte{0x00}, std::byte{0x00}, std::byte{0x80}});
    auto destination = std::vector<std::byte>{};
    auto const sourceFormat =
      PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Signed24PackedLe};

    auto const res =
      convertPcmEncoding(source, sourceFormat, signalFormat(sourceFormat), SampleEncoding::Float32Le, destination);

    REQUIRE(res);
    CHECK(destination == std::vector<std::byte>{std::byte{0xFE},
                                                std::byte{0xFF},
                                                std::byte{0x7F},
                                                std::byte{0x3F},
                                                std::byte{0x00},
                                                std::byte{0x00},
                                                std::byte{0x80},
                                                std::byte{0xBF}});
  }

  TEST_CASE("convertPcmEncoding - Float32 carrying an integer signal still rejects integer output")
  {
    auto const source = std::to_array<std::byte>({std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x3F}});
    auto destination = std::vector<std::byte>{};
    auto const sourceFormat = PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Float32Le};
    auto const sourceSignal =
      SignalFormat{.sampleRate = 48000, .channels = 1, .precisionBits = 16, .sampleKind = SampleKind::Integer};

    auto const res = convertPcmEncoding(source, sourceFormat, sourceSignal, SampleEncoding::Signed16Le, destination);

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::NotSupported);
    CHECK(destination.empty());
  }

  TEST_CASE("convertPcmEncoding - input must contain complete interleaved frames")
  {
    auto const source = std::to_array<std::byte>(
      {std::byte{0x01}, std::byte{0x00}, std::byte{0x02}, std::byte{0x00}, std::byte{0x03}, std::byte{0x00}});
    auto destination = std::vector<std::byte>{};
    auto const sourceFormat = PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le};

    auto const res =
      convertPcmEncoding(source, sourceFormat, signalFormat(sourceFormat), SampleEncoding::Signed32Le, destination);

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::InvalidInput);
    CHECK(destination.empty());
  }

  TEST_CASE("convertPcmEncoding - byte layout and logical signal must describe the same stream")
  {
    auto const source = std::to_array<std::byte>({std::byte{0x01}, std::byte{0x00}});
    auto destination = std::vector<std::byte>{};
    auto const sourceFormat = PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Signed16Le};
    auto const mismatchedSignal = SignalFormat{.sampleRate = 44100, .channels = 1, .precisionBits = 16};

    auto const res =
      convertPcmEncoding(source, sourceFormat, mismatchedSignal, SampleEncoding::Signed32Le, destination);

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::InvalidInput);
    CHECK(destination.empty());
  }

  TEST_CASE("convertPcmEncoding - same-count streams with different speaker masks are rejected")
  {
    // One stereo S16 frame: L = 0x1234, R = 0x5678, little-endian.
    auto const source = std::to_array<std::byte>({std::byte{0x34}, std::byte{0x12}, std::byte{0x78}, std::byte{0x56}});
    auto const sourceFormat =
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x3};
    std::uint32_t signalMask = 0x3;

    SECTION("a different identified mask on the same two channels")
    {
      signalMask = 0x5; // WAVE-style FL | FC, a complete alternate two-speaker identity.
    }

    SECTION("an unassigned signal mask is not inferred from the byte layout")
    {
      signalMask = 0U;
    }

    auto destination = std::vector<std::byte>{};
    auto const mismatchedSignal =
      SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = signalMask};

    auto const res =
      convertPcmEncoding(source, sourceFormat, mismatchedSignal, SampleEncoding::Signed32Le, destination);

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::InvalidInput);
    CHECK(destination.empty());
  }

  TEST_CASE("convertPcmEncoding - matching speaker masks convert exact stereo bytes")
  {
    // One stereo S16 frame: L = 0x1234, R = 0x5678, little-endian.
    auto const source = std::to_array<std::byte>({std::byte{0x34}, std::byte{0x12}, std::byte{0x78}, std::byte{0x56}});
    auto const sourceFormat =
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x3};
    auto const signal = SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 0x3};
    auto destination = std::vector<std::byte>{};

    SECTION("identical destination encoding copies the frame unchanged")
    {
      auto const res = convertPcmEncoding(source, sourceFormat, signal, SampleEncoding::Signed16Le, destination);

      REQUIRE(res);
      CHECK(destination == std::vector<std::byte>{std::byte{0x34}, std::byte{0x12}, std::byte{0x78}, std::byte{0x56}});
    }

    SECTION("widened destination preserves full-scale samples in their original slot order")
    {
      auto const res = convertPcmEncoding(source, sourceFormat, signal, SampleEncoding::Signed32Le, destination);

      REQUIRE(res);
      CHECK(destination == std::vector<std::byte>{std::byte{0x00},
                                                  std::byte{0x00},
                                                  std::byte{0x34},
                                                  std::byte{0x12},
                                                  std::byte{0x00},
                                                  std::byte{0x00},
                                                  std::byte{0x78},
                                                  std::byte{0x56}});
    }
  }
} // namespace ao::audio::test
