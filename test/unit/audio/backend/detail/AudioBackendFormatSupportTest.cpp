// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Device.h>
#include <ao/audio/Format.h>
#include <ao/audio/backend/detail/AudioBackendFormatSupport.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("AudioBackendFormatSupport - addSampleFormatCapability keeps unique formats",
            "[audio][unit][backend][format]")
  {
    auto caps = DeviceFormatCapabilities{};
    auto const cap = SampleFormatCapability{.bitDepth = 16, .validBits = 16, .isFloat = false};

    addSampleFormatCapability(caps, cap);
    CHECK(caps.sampleFormats.size() == 1);

    addSampleFormatCapability(caps, cap);
    CHECK(caps.sampleFormats.size() == 1);

    auto const floatingCap = SampleFormatCapability{.bitDepth = 32, .validBits = 32, .isFloat = true};
    addSampleFormatCapability(caps, floatingCap);
    CHECK(caps.sampleFormats.size() == 2);
  }

  TEST_CASE("AudioBackendFormatSupport - sample format capabilities update integer bit depths",
            "[audio][unit][backend][format]")
  {
    auto caps = DeviceFormatCapabilities{};
    auto const packedCap = SampleFormatCapability{.bitDepth = 32, .validBits = 24, .isFloat = false};

    addSampleFormatCapability(caps, packedCap);
    CHECK(caps.sampleFormats.size() == 1);
    CHECK(caps.bitDepths.empty());

    auto const cap16 = SampleFormatCapability{.bitDepth = 16, .validBits = 16, .isFloat = false};
    addSampleFormatCapability(caps, cap16);
    CHECK(caps.sampleFormats.size() == 2);
    CHECK(caps.bitDepths == std::vector<std::uint8_t>{16});
  }

  TEST_CASE("AudioBackendFormatSupport - current hardware container preserves requested signal precision",
            "[audio][unit][backend][format]")
  {
    auto const requested = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .validBits = 16};
    auto const currentHw = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 32, .validBits = 32};

    auto const graphFormat = preserveRequestedSignalPrecision(requested, currentHw);

    CHECK(graphFormat.bitDepth == 32);
    CHECK(graphFormat.validBits == 16);
  }
} // namespace ao::audio::backend::detail::test
