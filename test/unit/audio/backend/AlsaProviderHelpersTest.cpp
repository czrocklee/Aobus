// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/backend/detail/AlsaProviderHelpers.h>
#include <catch2/catch_test_macros.hpp>

using namespace ao::audio;
using namespace ao::audio::backend::detail;

TEST_CASE("AlsaProviderHelpers - Logic", "[audio][alsa][monitor]")
{
  SECTION("addSampleFormatCapability - Unique")
  {
    auto caps = DeviceCapabilities{};
    auto cap = SampleFormatCapability{.bitDepth = 16, .validBits = 16, .isFloat = false};

    addSampleFormatCapability(caps, cap);
    CHECK(caps.sampleFormats.size() == 1);

    // Duplicate
    addSampleFormatCapability(caps, cap);
    CHECK(caps.sampleFormats.size() == 1);

    // Different
    SampleFormatCapability cap2{.bitDepth = 32, .validBits = 32, .isFloat = true};
    addSampleFormatCapability(caps, cap2);
    CHECK(caps.sampleFormats.size() == 2);
  }

  SECTION("Enumerate - Basic Sanity")
  {
    // This will hit real ALSA, so we just check it doesn't crash
    auto devices = doAlsaEnumerate();
    for (auto const& dev : devices)
    {
      CHECK_FALSE(dev.id.empty());
      CHECK(dev.backendId == kBackendAlsa);
    }
  }
}
