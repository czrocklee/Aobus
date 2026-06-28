// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/backend/detail/AlsaPcmVolume.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace ao::audio::backend::detail;

TEST_CASE("AlsaPcmVolume - S16 unity gain preserves samples", "[audio][unit][alsa]")
{
  auto samples = std::vector<std::int16_t>{1000, -1000, 0, 32767, -32768};
  auto pcm = std::span{reinterpret_cast<std::byte*>(samples.data()), samples.size() * sizeof(std::int16_t)};

  applyAlsaSoftwareGain(pcm, 16, 16, false, 1.0F);

  CHECK(samples[0] == 1000);
  CHECK(samples[1] == -1000);
  CHECK(samples[2] == 0);
  CHECK(samples[3] == 32767);
  CHECK(samples[4] == -32768);
}

TEST_CASE("AlsaPcmVolume - S16 half gain scales samples", "[audio][unit][alsa]")
{
  auto samples = std::vector<std::int16_t>{1000, -1000, 0, 32766, -32766};
  auto pcm = std::span{reinterpret_cast<std::byte*>(samples.data()), samples.size() * sizeof(std::int16_t)};

  applyAlsaSoftwareGain(pcm, 16, 16, false, 0.5F);

  CHECK(samples[0] == 500);
  CHECK(samples[1] == -500);
  CHECK(samples[2] == 0);
  CHECK(samples[3] == 16383);
  CHECK(samples[4] == -16383);
}

TEST_CASE("AlsaPcmVolume - S16 zero gain mutes samples", "[audio][unit][alsa]")
{
  auto samples = std::vector<std::int16_t>{1000, -1000};
  auto pcm = std::span{reinterpret_cast<std::byte*>(samples.data()), samples.size() * sizeof(std::int16_t)};

  applyAlsaSoftwareGain(pcm, 16, 16, false, 0.0F);

  CHECK(samples[0] == 0);
  CHECK(samples[1] == 0);
}

TEST_CASE("AlsaPcmVolume - S32 half gain scales samples", "[audio][unit][alsa]")
{
  auto samples = std::vector<std::int32_t>{2000000, -2000000};
  auto pcm = std::span{reinterpret_cast<std::byte*>(samples.data()), samples.size() * sizeof(std::int32_t)};

  applyAlsaSoftwareGain(pcm, 32, 32, false, 0.5F);

  CHECK(samples[0] == 1000000);
  CHECK(samples[1] == -1000000);
}

TEST_CASE("AlsaPcmVolume - S24 in S32 half gain scales valid bits", "[audio][unit][alsa]")
{
  // 2^23 = 8388608
  auto samples = std::vector<std::int32_t>{4000000, -4000000, 8388607, -8388608};
  auto pcm = std::span{reinterpret_cast<std::byte*>(samples.data()), samples.size() * sizeof(std::int32_t)};

  applyAlsaSoftwareGain(pcm, 32, 24, false, 0.5F);

  CHECK(samples[0] == 2000000);
  CHECK(samples[1] == -2000000);
  CHECK(samples[2] == 4194304); // round(8388607 * 0.5) = 4194303.5 -> 4194304
  CHECK(samples[3] == -4194304);
}

TEST_CASE("AlsaPcmVolume - packed S24 half gain scales sign-extended samples", "[audio][unit][alsa]")
{
  // 0x7FFFFF = 8388607
  // 0x800000 = -8388608
  auto samples = std::vector<std::uint8_t>{
    0x00,
    0x00,
    0x01, // 65536
    0x00,
    0x00,
    0xFF, // -65536
    0xFF,
    0xFF,
    0x7F, // 8388607
    0x00,
    0x00,
    0x80 // -8388608
  };
  auto pcm = std::span{reinterpret_cast<std::byte*>(samples.data()), samples.size()};

  applyAlsaSoftwareGain(pcm, 24, 24, true, 0.5F);

  // 65536 * 0.5 = 32768 (0x00, 0x80, 0x00)
  CHECK(samples[0] == 0x00);
  CHECK(samples[1] == 0x80);
  CHECK(samples[2] == 0x00);

  // -65536 * 0.5 = -32768 (0x00, 0x80, 0xFF)
  CHECK(samples[3] == 0x00);
  CHECK(samples[4] == 0x80);
  CHECK(samples[5] == 0xFF);

  // 8388607 * 0.5 = 4194304 (0x00, 0x00, 0x40)
  CHECK(samples[6] == 0x00);
  CHECK(samples[7] == 0x00);
  CHECK(samples[8] == 0x40);

  // -8388608 * 0.5 = -4194304 (0x00, 0x00, 0xC0)
  CHECK(samples[9] == 0x00);
  CHECK(samples[10] == 0x00);
  CHECK(samples[11] == 0xC0);
}
