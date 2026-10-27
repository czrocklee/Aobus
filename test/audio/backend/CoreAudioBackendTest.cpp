// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/CoreAudioBackend.h"

#include "test/audio/BackendTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace ao::audio::backend::test
{
  TEST_CASE("CoreAudioBackend - caches software volume and mute before native open")
  {
    auto backend = CoreAudioBackend{
      Device{.id = DeviceId{"synthetic-uid"}, .displayName = "Synthetic Output", .backendId = kBackendCoreAudio},
      kProfileShared};
    CHECK(backend.backendId() == kBackendCoreAudio);
    CHECK(backend.profileId() == kProfileShared);
    CHECK_FALSE(backend.queryProperty(PropertyId::Volume).isAvailable);

    backend.start();
    backend.pause();
    backend.resume();
    backend.flush();
    backend.stop();
    backend.close();
    CHECK_FALSE(backend.queryProperty(PropertyId::Volume).isAvailable);

    REQUIRE(backend.set(props::kVolume, 0.4F));
    REQUIRE(backend.set(props::kMuted, true));
    auto const volumeRes = backend.get(props::kVolume);
    auto const mutedRes = backend.get(props::kMuted);
    REQUIRE(volumeRes);
    REQUIRE(mutedRes);
    CHECK(*volumeRes == 0.4F);
    CHECK(*mutedRes);
  }

  TEST_CASE("CoreAudioBackend - an unrepresentable source is refused as a track incompatibility before native access")
  {
    // The UID names no native device, so a regressed guard could only surface
    // a device lookup failure here, never an audio unit.
    auto backend = CoreAudioBackend{
      Device{.id = DeviceId{"synthetic-uid"}, .displayName = "Synthetic Output", .backendId = kBackendCoreAudio},
      kProfileShared};
    auto target = ao::audio::test::NoopRenderTarget{};

    SECTION("a 33-bit integer signal has no lossless encoding at all")
    {
      auto const openedRes = backend.open(
        {.sampleRate = 44100, .channels = 2, .precisionBits = 33, .sampleKind = SampleKind::Integer}, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::FormatRejected);
      CHECK(openedRes.error().error.message == "No lossless Core Audio client PCM encoding is available");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    }

    SECTION("a non-32-bit float signal has no lossless encoding at all")
    {
      auto const openedRes = backend.open(
        {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::FloatingPoint}, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::FormatRejected);
      CHECK(openedRes.error().error.message == "No lossless Core Audio client PCM encoding is available");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    }
  }

  TEST_CASE("CoreAudioBackend - zero geometry is refused as invalid input before native access")
  {
    // The UID names no native device, so a regressed guard could only surface
    // a device lookup failure here, never an audio unit.
    auto backend = CoreAudioBackend{
      Device{.id = DeviceId{"synthetic-uid"}, .displayName = "Synthetic Output", .backendId = kBackendCoreAudio},
      kProfileShared};
    auto target = ao::audio::test::NoopRenderTarget{};

    SECTION("a zero sample rate is invalid geometry")
    {
      auto const sourceFormat =
        SignalFormat{.sampleRate = 0, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer};
      auto const openedRes = backend.open(sourceFormat, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "Core Audio PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK_FALSE(backend.prewarmFormatHint(sourceFormat).has_value());
    }

    SECTION("a zero channel count is invalid geometry")
    {
      auto const sourceFormat =
        SignalFormat{.sampleRate = 44100, .channels = 0, .precisionBits = 16, .sampleKind = SampleKind::Integer};
      auto const openedRes = backend.open(sourceFormat, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "Core Audio PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK_FALSE(backend.prewarmFormatHint(sourceFormat).has_value());
    }
  }

  TEST_CASE("CoreAudioBackend - geometry refusal outranks precision refusal")
  {
    auto backend = CoreAudioBackend{
      Device{.id = DeviceId{"synthetic-uid"}, .displayName = "Synthetic Output", .backendId = kBackendCoreAudio},
      kProfileShared};
    auto target = ao::audio::test::NoopRenderTarget{};

    SECTION("an unrepresentable source with no channels still reports the geometry refusal")
    {
      auto const openedRes = backend.open(
        {.sampleRate = 44100, .channels = 0, .precisionBits = 33, .sampleKind = SampleKind::Integer}, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "Core Audio PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    }
  }

  TEST_CASE("CoreAudioBackend - locally unsupported identity is refused before native access and prewarm")
  {
    auto backend = CoreAudioBackend{
      Device{.id = DeviceId{"synthetic-uid"}, .displayName = "Synthetic Output", .backendId = kBackendCoreAudio},
      kProfileShared};
    auto target = ::ao::audio::test::NoopRenderTarget{};
    // Partial, excess and reserved identities cannot provision a permutation.
    auto const speakerMask = GENERATE(0x1U, 0x7U, 0x80000001U);
    auto const sourceFormat =
      SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 16, .speakerMask = speakerMask};

    CHECK_FALSE(backend.prewarmFormatHint(sourceFormat));
    auto const openedRes = backend.open(sourceFormat, target);

    REQUIRE_FALSE(openedRes);
    CHECK(openedRes.error().error.code == Error::Code::FormatRejected);
    CHECK(openedRes.error().error.message == "Core Audio cannot identify the source channel set");
    CHECK(openedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
  }

  TEST_CASE("CoreAudioBackend - an identified source reaches ordinary device lookup instead of a blanket layout ban")
  {
    auto backend = CoreAudioBackend{
      Device{.id = DeviceId{"synthetic-uid"}, .displayName = "Synthetic Output", .backendId = kBackendCoreAudio},
      kProfileShared};
    auto target = ao::audio::test::NoopRenderTarget{};
    // Complete alternate roles are not made stereo merely from channel count.
    auto const speakerMask = GENERATE(0x3U, 0x30U);
    auto const openedRes = backend.open({.sampleRate = 44100,
                                         .channels = 2,
                                         .precisionBits = 16,
                                         .sampleKind = SampleKind::Integer,
                                         .speakerMask = speakerMask},
                                        target);
    REQUIRE_FALSE(openedRes);
    CHECK(openedRes.error().error.code == Error::Code::DeviceNotFound);
    CHECK(openedRes.error().error.message == "Core Audio output device is no longer available");
    CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    backend.close();
  }

  TEST_CASE("CoreAudioBackend - local refusals retain geometry then precision then identity precedence")
  {
    auto backend = CoreAudioBackend{
      Device{.id = DeviceId{"synthetic-uid"}, .displayName = "Synthetic Output", .backendId = kBackendCoreAudio},
      kProfileShared};
    auto target = ao::audio::test::NoopRenderTarget{};
    auto source = SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 33, .speakerMask = 0x1};
    bool isGeometryFailure = false;

    SECTION("zero rate wins over precision and partial identity")
    {
      source.sampleRate = 0;
      isGeometryFailure = true;
    }

    SECTION("zero count wins over precision and reserved identity")
    {
      source.channels = 0;
      source.speakerMask = 0x80000001;
      isGeometryFailure = true;
    }

    SECTION("33-bit precision wins over partial identity")
    {
    }

    SECTION("nonrepresentable float precision wins over reserved identity")
    {
      source.precisionBits = 16;
      source.sampleKind = SampleKind::FloatingPoint;
      source.speakerMask = 0x80000001;
    }

    SECTION("zero precision has no lossless encoding")
    {
      source.precisionBits = 0;
      source.speakerMask = 0;
    }

    CHECK_FALSE(backend.prewarmFormatHint(source));
    auto const openedRes = backend.open(source, target);
    REQUIRE_FALSE(openedRes);
    CHECK(openedRes.error().error.code ==
          (isGeometryFailure ? Error::Code::InvalidInput : Error::Code::FormatRejected));
    CHECK(openedRes.error().error.message == (isGeometryFailure
                                                ? "Core Audio PCM formats require a sample rate and channels"
                                                : "No lossless Core Audio client PCM encoding is available"));
    CHECK(openedRes.error().cause == (isGeometryFailure ? BackendOpenFailure::Cause::OutputFailure
                                                        : BackendOpenFailure::Cause::TrackRouteIncompatible));
  }

  TEST_CASE("CoreAudioBackend - prewarm predicts the lossless mode without native negotiation")
  {
    // prewarmFormatHint is non-blocking and performs no native open or
    // negotiation, so a synthetic device observes only local admission policy.
    auto backend = CoreAudioBackend{
      Device{.id = DeviceId{"synthetic-uid"}, .displayName = "Synthetic Output", .backendId = kBackendCoreAudio},
      kProfileShared};

    SECTION("an ordinary representable source retains its precision")
    {
      auto const optHint = backend.prewarmFormatHint(
        {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer});

      REQUIRE(optHint);
      CHECK(optHint->sampleRate == 44100U);
      CHECK(optHint->channels == 2U);
      CHECK(optHint->encoding == SampleEncoding::Signed16Le);
    }
  }

  TEST_CASE("CoreAudioBackend - prewarm retains identified source eligibility without native negotiation")
  {
    auto backend = CoreAudioBackend{
      Device{.id = DeviceId{"synthetic-uid"}, .displayName = "Synthetic Output", .backendId = kBackendCoreAudio},
      kProfileShared};

    SECTION("identified stereo retains its canonical mask")
    {
      auto const optHint =
        backend.prewarmFormatHint({.sampleRate = 44100, .channels = 2, .precisionBits = 16, .speakerMask = 0x3});
      REQUIRE(optHint);
      CHECK(optHint->sampleRate == 44100U);
      CHECK(optHint->channels == 2U);
      CHECK(optHint->encoding == SampleEncoding::Signed16Le);
      CHECK(optHint->speakerMask == 0x3U);
    }

    SECTION("identified multichannel retains its lossless geometry")
    {
      auto const optHint =
        backend.prewarmFormatHint({.sampleRate = 48000, .channels = 6, .precisionBits = 24, .speakerMask = 0x3F});
      REQUIRE(optHint);
      CHECK(optHint->sampleRate == 48000U);
      CHECK(optHint->channels == 6U);
      CHECK(optHint->encoding == SampleEncoding::Signed24PackedLe);
      CHECK(optHint->speakerMask == 0x3FU);
    }
  }
} // namespace ao::audio::backend::test
