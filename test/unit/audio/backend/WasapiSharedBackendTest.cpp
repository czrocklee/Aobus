// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/backend/WasapiSharedBackend.h"

#include "lib/audio/backend/detail/WasapiError.h"
#include "lib/audio/backend/detail/WasapiFormat.h"
#include "lib/audio/backend/detail/WasapiGraphRegistry.h"
#include "lib/audio/backend/detail/WasapiRenderBuffer.h"
#include "test/unit/audio/BackendTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Property.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <vector>

namespace ao::audio::backend::test
{
  namespace
  {
    Device testDevice()
    {
      return {.id = DeviceId{"endpoint-a"}, .displayName = "Test endpoint", .backendId = kBackendWasapi};
    }
  } // namespace

  TEST_CASE("WasapiSharedBackend - identity and unopened lifecycle are stable", "[audio][unit][wasapi][backend]")
  {
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};

    CHECK(backend.backendId() == kBackendWasapi);
    CHECK(backend.profileId() == kProfileShared);

    backend.start();
    backend.pause();
    backend.resume();
    backend.flush();
    backend.stop();
    backend.close();
  }

  TEST_CASE("WasapiSharedBackend - cached properties round trip before opening a device",
            "[audio][unit][wasapi][backend]")
  {
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};

    REQUIRE(backend.set(props::kVolume, 0.25F));
    REQUIRE(backend.set(props::kMuted, true));
    auto const volumeRes = backend.get(props::kVolume);
    auto const mutedRes = backend.get(props::kMuted);

    REQUIRE(volumeRes);
    REQUIRE(mutedRes);
    CHECK(*volumeRes == 0.25F);
    CHECK(*mutedRes);

    auto const volumeInfo = backend.queryProperty(PropertyId::Volume);
    auto const mutedInfo = backend.queryProperty(PropertyId::Muted);
    CHECK(volumeInfo.canRead);
    CHECK(volumeInfo.canWrite);
    CHECK_FALSE(volumeInfo.isAvailable);
    CHECK_FALSE(volumeInfo.emitsChangeNotifications);
    CHECK_FALSE(volumeInfo.isHardwareAssisted);
    CHECK(mutedInfo == volumeInfo);
  }

  TEST_CASE("WasapiSharedBackend - unopened property intent does not claim applied session state",
            "[audio][unit][wasapi][backend]")
  {
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto received = std::vector<flow::Graph>{};
    auto sub = registryPtr->subscribe("endpoint-a", [&](flow::Graph const& graph) { received.push_back(graph); });
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};

    REQUIRE(backend.set(props::kVolume, -0.5F));
    auto volumeRes = backend.get(props::kVolume);
    REQUIRE(volumeRes);
    CHECK(*volumeRes == 0.0F);
    REQUIRE(received.size() == 2);
    REQUIRE(received.back().nodes.size() == 2);
    CHECK_FALSE(received.back().nodes[1].softwareVolumeNotUnity);
    CHECK(received.back().nodes[1].maxSoftwareGain == 1.0F);

    auto const unappliedGraph = received.back();
    CHECK(received.front() == unappliedGraph);
    REQUIRE(backend.set(props::kVolume, 1.5F));
    volumeRes = backend.get(props::kVolume);
    REQUIRE(volumeRes);
    CHECK(*volumeRes == 1.0F);
    // Changed unopened intent leaves the applied-session graph unchanged, so it is not redelivered.
    REQUIRE(received.size() == 2);
    CHECK(received.back() == unappliedGraph);
    CHECK_FALSE(received.back().nodes[1].softwareVolumeNotUnity);
    CHECK(received.back().nodes[1].maxSoftwareGain == 1.0F);

    backend.close();
    REQUIRE(received.size() == 3);
    CHECK(received.back().nodes.empty());
    CHECK(received.back().connections.empty());
  }

  TEST_CASE(
    "WasapiSharedBackend - an unrepresentable source is refused as a track incompatibility before endpoint access",
    "[audio][unit][wasapi][backend]")
  {
    // The endpoint id names no real endpoint, so a regressed guard could only
    // surface an endpoint lookup failure here, never a native handle.
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};
    auto target = ::ao::audio::test::NoopRenderTarget{};

    SECTION("a 33-bit integer signal has no lossless encoding at all")
    {
      auto const sourceFormat =
        SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 33, .sampleKind = SampleKind::Integer};
      auto const openedRes = backend.open(sourceFormat, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::FormatRejected);
      CHECK(openedRes.error().error.message == "No lossless WASAPI input encoding is available");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
      CHECK_FALSE(backend.prewarmFormatHint(sourceFormat).has_value());
    }

    SECTION("a non-32-bit float signal has no lossless encoding at all")
    {
      auto const openedRes = backend.open(
        SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::FloatingPoint},
        target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::FormatRejected);
      CHECK(openedRes.error().error.message == "No lossless WASAPI input encoding is available");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    }
  }

  TEST_CASE("WasapiSharedBackend - zero geometry is refused as invalid input before endpoint access",
            "[audio][unit][wasapi][backend]")
  {
    // The endpoint id names no real endpoint, so a regressed guard could only
    // surface an endpoint lookup failure here, never a native handle.
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};
    auto target = ::ao::audio::test::NoopRenderTarget{};

    SECTION("a zero sample rate is invalid geometry")
    {
      auto const sourceFormat = SignalFormat{.sampleRate = 0, .channels = 2, .precisionBits = 16};
      auto const openedRes = backend.open(sourceFormat, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "WASAPI PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK_FALSE(backend.prewarmFormatHint(sourceFormat).has_value());
    }

    SECTION("a zero channel count is invalid geometry")
    {
      auto const sourceFormat = SignalFormat{.sampleRate = 48000, .channels = 0, .precisionBits = 16};
      auto const openedRes = backend.open(sourceFormat, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "WASAPI PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK_FALSE(backend.prewarmFormatHint(sourceFormat).has_value());
    }
  }

  TEST_CASE("WasapiSharedBackend - geometry refusal outranks precision refusal", "[audio][unit][wasapi][backend]")
  {
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};
    auto target = ::ao::audio::test::NoopRenderTarget{};

    SECTION("an unrepresentable source with no channels still reports the geometry refusal")
    {
      auto const openedRes =
        backend.open(SignalFormat{.sampleRate = 48000, .channels = 0, .precisionBits = 33}, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "WASAPI PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    }
  }

  TEST_CASE("WasapiSharedBackend - prewarm predicts the lossless mode without native negotiation",
            "[audio][unit][wasapi][backend]")
  {
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};

    SECTION("an ordinary representable source retains its precision")
    {
      auto const optHint =
        backend.prewarmFormatHint(SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16});

      REQUIRE(optHint);
      CHECK(optHint->sampleRate == 48000U);
      CHECK(optHint->channels == 2U);
      CHECK(optHint->encoding == SampleEncoding::Signed16Le);
    }
  }

  TEST_CASE("prepareWasapiRenderPacket pads short PCM without advancing silence", "[audio][unit][wasapi][render]")
  {
    auto buffer = std::array<std::byte, 16>{};
    std::ranges::fill(buffer, std::byte{0x5a});

    auto const packet = detail::prepareWasapiRenderPacket(
      buffer, 4, {.bytesWritten = 8, .positionFrameOffset = 1, .positionFrames = 1, .drained = false});

    CHECK(packet.renderedFrames == 2);
    CHECK(packet.framesToRelease == 4);
    CHECK(packet.positionFrames == 1);
    CHECK(packet.underrun);
    CHECK_FALSE(packet.drained);

    for (std::size_t index = 0; index < buffer.size(); ++index)
    {
      CHECK(buffer[index] == (index < 8 ? std::byte{0x5a} : std::byte{0}));
    }
  }

  TEST_CASE("prepareWasapiRenderPacket releases drained short and empty packets correctly",
            "[audio][unit][wasapi][render]")
  {
    auto buffer = std::array<std::byte, 16>{};
    std::ranges::fill(buffer, std::byte{0x5a});

    SECTION("a final PCM prefix commits a complete silent-padded packet")
    {
      auto const packet = detail::prepareWasapiRenderPacket(buffer, 4, {.bytesWritten = 4, .drained = true});

      CHECK(packet.renderedFrames == 1);
      CHECK(packet.framesToRelease == 4);
      CHECK_FALSE(packet.underrun);
      CHECK(packet.drained);
      CHECK(buffer.front() == std::byte{0x5a});
      CHECK(buffer.back() == std::byte{0});
    }

    SECTION("an empty drained packet is released without a commit")
    {
      auto const packet = detail::prepareWasapiRenderPacket(buffer, 4, {.bytesWritten = 0, .drained = true});

      CHECK(packet.renderedFrames == 0);
      CHECK(packet.framesToRelease == 0);
      CHECK_FALSE(packet.underrun);
      CHECK(packet.drained);
      CHECK(buffer.back() == std::byte{0x5a});
    }
  }

  TEST_CASE("formatFromWaveFormat preserves endpoint mix precision", "[audio][unit][wasapi][format]")
  {
    auto wave = WAVEFORMATEXTENSIBLE{};
    wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wave.Format.nChannels = 2;
    wave.Format.nSamplesPerSec = 48000;
    wave.Format.wBitsPerSample = 32;
    wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    // WAVEFORMATEXTENSIBLE exposes valid precision through the Windows SDK union.
    wave.Samples.wValidBitsPerSample = 24; // NOLINT(cppcoreguidelines-pro-type-union-access)
    wave.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

    auto const optFormat = detail::formatFromWaveFormat(wave.Format);

    REQUIRE(optFormat);
    CHECK(optFormat->sampleRate == 48000);
    CHECK(optFormat->channels == 2);
    CHECK(optFormat->encoding == SampleEncoding::Signed24In32Le);
  }

  TEST_CASE("formatFromWaveFormat leaves unsupported endpoint encoding unknown", "[audio][unit][wasapi][format]")
  {
    auto wave = WAVEFORMATEX{};
    wave.wFormatTag = WAVE_FORMAT_MPEGLAYER3;
    wave.nChannels = 2;
    wave.nSamplesPerSec = 48000;
    wave.wBitsPerSample = 16;

    CHECK_FALSE(detail::formatFromWaveFormat(wave));
  }

  TEST_CASE("wasapiInitializeFailureCause maps only the unsupported format to a route mismatch",
            "[audio][unit][wasapi][backend]")
  {
    struct CauseCase final
    {
      ::HRESULT status = S_OK;
      BackendOpenFailure::Cause cause = BackendOpenFailure::Cause::OutputFailure;
    };

    auto const causeCases =
      std::array{CauseCase{AUDCLNT_E_UNSUPPORTED_FORMAT, BackendOpenFailure::Cause::TrackRouteIncompatible},
                 CauseCase{E_INVALIDARG, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{E_OUTOFMEMORY, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{E_FAIL, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{AUDCLNT_E_DEVICE_INVALIDATED, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{AUDCLNT_E_ENDPOINT_CREATE_FAILED, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{AUDCLNT_E_DEVICE_IN_USE, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{AUDCLNT_E_SERVICE_NOT_RUNNING, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{AUDCLNT_E_RESOURCES_INVALIDATED, BackendOpenFailure::Cause::OutputFailure},
                 // A failing status unknown to the Initialize policy stays terminal.
                 CauseCase{E_NOTIMPL, BackendOpenFailure::Cause::OutputFailure}};

    for (auto const& causeCase : causeCases)
    {
      CAPTURE(causeCase.status);
      CHECK(detail::wasapiInitializeFailureCause(causeCase.status) == causeCase.cause);
    }
  }
} // namespace ao::audio::backend::test
