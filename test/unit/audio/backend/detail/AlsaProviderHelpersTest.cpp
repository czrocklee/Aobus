// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/Backend.h>
#include <ao/audio/backend/detail/AudioBackendShared.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("AlsaProviderHelpers - Logic", "[audio][unit][alsa][monitor]")
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
      auto const cap2 = SampleFormatCapability{.bitDepth = 32, .validBits = 32, .isFloat = true};
      addSampleFormatCapability(caps, cap2);
      CHECK(caps.sampleFormats.size() == 2);
    }

    // NOTE: doAlsaEnumerate() hits real ALSA hardware and is tested in
    // test/integration/audio/backend/ instead.
  }
} // namespace ao::audio::backend::detail::test
