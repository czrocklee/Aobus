// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/WasapiSharedBackend.h"
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>

namespace ao::audio::backend::test
{
  namespace
  {
    Device testDevice()
    {
      return {.id = DeviceId{"endpoint-a"}, .displayName = "Test endpoint", .backendId = kBackendWasapi};
    }
  } // namespace

  TEST_CASE("WasapiSharedBackend - identical valid PCM modes are reusable before opening",
            "[audio][unit][wasapi][backend]")
  {
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared};
    Backend const& polymorphicBackend = backend;
    auto const modes = std::array{
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le},
      PcmFormat{.sampleRate = 44100, .channels = 2, .encoding = SampleEncoding::Signed24PackedLe, .speakerMask = 3},
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed24In32Le, .speakerMask = 3},
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed32Le, .speakerMask = 0x30},
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Float32Le, .speakerMask = 5},
      PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Signed16Le, .speakerMask = 4},
      PcmFormat{.sampleRate = 48000, .channels = 4, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x33}};

    for (auto const& mode : modes)
    {
      CAPTURE(mode.sampleRate, mode.channels, mode.encoding, mode.speakerMask);
      CHECK(backend.canReuseFormat(mode, mode));
      CHECK(polymorphicBackend.canReuseFormat(mode, mode));
    }
  }

  TEST_CASE("WasapiSharedBackend - zero and FL FR stereo reuse is bidirectional for every encoding",
            "[audio][unit][wasapi][backend]")
  {
    // This immutable transport policy needs no installed native client. The
    // synthetic endpoint is never opened; this does not test device liveness.
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared};
    Backend const& polymorphicBackend = backend;
    auto const encodings = std::array{SampleEncoding::Signed16Le,
                                      SampleEncoding::Signed24PackedLe,
                                      SampleEncoding::Signed24In32Le,
                                      SampleEncoding::Signed32Le,
                                      SampleEncoding::Float32Le};

    for (auto const encoding : encodings)
    {
      CAPTURE(encoding);
      auto const first = PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = encoding, .speakerMask = 0};
      auto const second = PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = encoding, .speakerMask = 3};

      CHECK(backend.canReuseFormat(first, second));
      CHECK(backend.canReuseFormat(second, first));
      CHECK(polymorphicBackend.canReuseFormat(first, second));
      CHECK(polymorphicBackend.canReuseFormat(second, first));
    }
  }

  TEST_CASE("WasapiSharedBackend - rate count and encoding changes prevent stereo identity reuse",
            "[audio][unit][wasapi][backend]")
  {
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared};
    Backend const& polymorphicBackend = backend;
    auto first = PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le};
    auto second =
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 3};

    SECTION("sample rate differs")
    {
      second.sampleRate = 44100;
    }

    SECTION("channel count differs")
    {
      second.channels = 4;
    }

    SECTION("integer precision differs")
    {
      second.encoding = SampleEncoding::Signed32Le;
    }

    SECTION("equal precision with different container widths is not reusable")
    {
      first.encoding = SampleEncoding::Signed24PackedLe;
      second.encoding = SampleEncoding::Signed24In32Le;
    }

    SECTION("equal container widths with different integer precision are not reusable")
    {
      first.encoding = SampleEncoding::Signed24In32Le;
      second.encoding = SampleEncoding::Signed32Le;
    }

    SECTION("equal container widths with different sample kinds are not reusable")
    {
      first.encoding = SampleEncoding::Signed32Le;
      second.encoding = SampleEncoding::Float32Le;
    }

    CHECK_FALSE(backend.canReuseFormat(first, second));
    CHECK_FALSE(backend.canReuseFormat(second, first));
    CHECK_FALSE(polymorphicBackend.canReuseFormat(first, second));
    CHECK_FALSE(polymorphicBackend.canReuseFormat(second, first));
  }

  TEST_CASE("WasapiSharedBackend - identity aliases do not extend to mono multichannel or other roles",
            "[audio][unit][wasapi][backend]")
  {
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared};
    Backend const& polymorphicBackend = backend;
    auto first = PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le};
    auto second =
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 3};

    SECTION("unidentified mono and centered mono are not aliases")
    {
      first.channels = 1;
      second.channels = 1;
      second.speakerMask = 4;
    }

    SECTION("the zero and FL FR pair requires more than a mono channel count")
    {
      first.channels = 1;
      second.channels = 1;
    }

    SECTION("unidentified quad and identified quad are not aliases")
    {
      first.channels = 4;
      second.channels = 4;
      second.speakerMask = 0x33;
    }

    SECTION("the zero and FL FR pair is not a multichannel alias")
    {
      first.channels = 4;
      second.channels = 4;
    }

    SECTION("unidentified stereo does not alias rear stereo")
    {
      second.speakerMask = 0x30;
    }

    SECTION("unidentified stereo does not alias front left and center")
    {
      second.speakerMask = 5;
    }

    SECTION("front stereo does not alias rear stereo")
    {
      first.speakerMask = 3;
      second.speakerMask = 0x30;
    }

    SECTION("front stereo does not alias front left and center")
    {
      first.speakerMask = 3;
      second.speakerMask = 5;
    }

    SECTION("a partial stereo role set is not an alias")
    {
      second.speakerMask = 1;
    }

    CHECK_FALSE(backend.canReuseFormat(first, second));
    CHECK_FALSE(backend.canReuseFormat(second, first));
    CHECK_FALSE(polymorphicBackend.canReuseFormat(first, second));
    CHECK_FALSE(polymorphicBackend.canReuseFormat(second, first));
  }

  TEST_CASE("WasapiSharedBackend - invalid stereo alias geometry is refused before opening",
            "[audio][unit][wasapi][backend]")
  {
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared};
    Backend const& polymorphicBackend = backend;
    auto first = PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le};
    auto second =
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 3};

    // Only the differing-mask alias is being tested. Exact identity follows
    // the base policy; this query is not a general format admission validator.
    SECTION("neither alias has a sample rate")
    {
      first.sampleRate = 0;
      second.sampleRate = 0;
    }

    SECTION("neither alias has channels")
    {
      first.channels = 0;
      second.channels = 0;
    }

    SECTION("neither alias has a concrete encoding")
    {
      first.encoding = SampleEncoding::Unknown;
      second.encoding = SampleEncoding::Unknown;
    }

    CHECK_FALSE(backend.canReuseFormat(first, second));
    CHECK_FALSE(backend.canReuseFormat(second, first));
    CHECK_FALSE(polymorphicBackend.canReuseFormat(first, second));
    CHECK_FALSE(polymorphicBackend.canReuseFormat(second, first));
  }
} // namespace ao::audio::backend::test
