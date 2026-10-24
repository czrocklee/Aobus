// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/WasapiFormat.h"
#include "lib/audio/backend/detail/WasapiFormatObservation.h"
#include "test/unit/audio/backend/WasapiFormatTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace ao::audio::backend::detail::test
{
  namespace
  {
    struct MixAllocation final
    {
      WAVEFORMATEXTENSIBLE wave{};
      std::uint32_t* releases = nullptr;
    };

    // The SDK's nested first-member descriptors are pointer-interconvertible
    // with the allocation. The tracked release uses CoTaskMemFree's native ABI;
    // it observes our ownership boundary, not the Windows allocator itself.
    static_assert(std::is_standard_layout_v<MixAllocation>);
    static_assert(std::is_standard_layout_v<WAVEFORMATEXTENSIBLE>);
    static_assert(std::is_standard_layout_v<WAVEFORMATEX>);

    void WINAPI releaseMixAllocation(void* raw) noexcept
    {
      auto allocationPtr = std::unique_ptr<MixAllocation>{static_cast<MixAllocation*>(raw)};
      ++*allocationPtr->releases;
    }

    WaveFormatPtr makeMixAllocation(std::uint32_t& releases)
    {
      auto allocationPtr = std::make_unique<MixAllocation>();
      allocationPtr->wave = ::ao::audio::backend::test::extensibleFloatStereoWave();
      allocationPtr->releases = &releases;
      auto* wave = &allocationPtr.release()->wave.Format;
      return WaveFormatPtr{wave, &releaseMixAllocation};
    }
  } // namespace

  TEST_CASE("observeMixFormat releases successful query storage after observation", "[audio][unit][wasapi][format]")
  {
    std::uint32_t releases = 0;
    auto wavePtr = makeMixAllocation(releases);
    bool hasFormat = true;

    SECTION("a readable engine descriptor retains its independent facts")
    {
    }

    SECTION("a short successful descriptor returns its storage fault")
    {
      wavePtr->cbSize = 0;
      hasFormat = false;
    }

    auto const observationRes = observeMixFormat(S_OK, std::move(wavePtr));
    CHECK(releases == 1U);
    CHECK_FALSE(wavePtr);

    if (!hasFormat)
    {
      REQUIRE_FALSE(observationRes);
      CHECK(observationRes.error().code == Error::Code::FormatRejected);
      CHECK(observationRes.error().message == "WASAPI GetMixFormat returned a short extensible descriptor");
      return;
    }

    REQUIRE(observationRes);
    CHECK(observationRes->sampleRate == 48000U);
    CHECK(observationRes->channels == 2U);
    CHECK(observationRes->speakerMask == 0x3U);
    REQUIRE(observationRes->optFormat);
    auto const expectedFormat =
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Float32Le, .speakerMask = 0x3};
    CHECK(*observationRes->optFormat == expectedFormat);
  }

  TEST_CASE("observeMixFormat rejects successful null storage", "[audio][unit][wasapi][format]")
  {
    auto const observationRes = observeMixFormat(S_OK, WaveFormatPtr{nullptr, &releaseMixAllocation});

    REQUIRE_FALSE(observationRes);
    CHECK(observationRes.error().code == Error::Code::FormatRejected);
    CHECK(observationRes.error().message == "WASAPI GetMixFormat succeeded without a descriptor");
  }

  TEST_CASE("observeMixFormat drops failed query facts and releases any returned storage",
            "[audio][unit][wasapi][format]")
  {
    std::uint32_t releases = 0;
    auto wavePtr = WaveFormatPtr{nullptr, &releaseMixAllocation};
    std::uint32_t expectedReleases = 0;

    SECTION("a failed query with no descriptor is unavailable")
    {
    }

    SECTION("a failed query with malformed storage never parses that descriptor")
    {
      wavePtr = makeMixAllocation(releases);
      wavePtr->cbSize = 0;
      expectedReleases = 1;
    }

    auto const observationRes = observeMixFormat(E_FAIL, std::move(wavePtr));
    CHECK(releases == expectedReleases);
    CHECK_FALSE(wavePtr);
    REQUIRE(observationRes);
    CHECK(observationRes->sampleRate == 0U);
    CHECK(observationRes->channels == 0U);
    CHECK(observationRes->speakerMask == 0U);
    CHECK_FALSE(observationRes->optFormat);
  }
} // namespace ao::audio::backend::detail::test
