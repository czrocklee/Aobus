// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/Device.h>
#include <ao/audio/backend/detail/AudioBackendShared.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("AlsaProviderHelpers - addSampleFormatCapability keeps unique formats", "[audio][unit][alsa][monitor]")
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

    // NOTE: enumerateAlsaPlaybackDevices() hits real ALSA hardware and is tested in
    // test/integration/audio/backend/ instead.
  }

  TEST_CASE("AlsaProviderHelpers - committedPositionFrames handles partial cross-boundary commits",
            "[audio][unit][alsa][monitor]")
  {
    CHECK(committedPositionFrames(0, 2, 2) == 0);
    CHECK(committedPositionFrames(1, 2, 2) == 0);
    CHECK(committedPositionFrames(2, 2, 2) == 0);
    CHECK(committedPositionFrames(3, 2, 2) == 1);
    CHECK(committedPositionFrames(4, 2, 2) == 2);
    CHECK(committedPositionFrames(5, 2, 2) == 2);

    CHECK(committedPositionFrames(3, 0, 4) == 3);
  }
} // namespace ao::audio::backend::detail::test
