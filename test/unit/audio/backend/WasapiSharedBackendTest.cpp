// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Property.h>
#include <ao/audio/backend/WasapiSharedBackend.h>
#include <ao/audio/backend/detail/WasapiFormat.h>
#include <ao/audio/backend/detail/WasapiGraphRegistry.h>
#include <ao/audio/backend/detail/WasapiRenderBuffer.h>
#include <ao/audio/flow/Graph.h>

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
    auto const volume = backend.get(props::kVolume);
    auto const muted = backend.get(props::kMuted);

    REQUIRE(volume);
    REQUIRE(muted);
    CHECK(*volume == 0.25F);
    CHECK(*muted);

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
    auto volume = backend.get(props::kVolume);
    REQUIRE(volume);
    CHECK(*volume == 0.0F);
    REQUIRE(received.size() == 2);
    REQUIRE(received.back().nodes.size() == 2);
    CHECK_FALSE(received.back().nodes[1].softwareVolumeNotUnity);
    CHECK(received.back().nodes[1].maxSoftwareGain == 1.0F);

    REQUIRE(backend.set(props::kVolume, 1.5F));
    volume = backend.get(props::kVolume);
    REQUIRE(volume);
    CHECK(*volume == 1.0F);
    REQUIRE(received.size() == 3);
    CHECK_FALSE(received.back().nodes[1].softwareVolumeNotUnity);
    CHECK(received.back().nodes[1].maxSoftwareGain == 1.0F);

    backend.close();
    REQUIRE(received.size() == 4);
    CHECK(received.back().nodes.empty());
    CHECK(received.back().connections.empty());
  }

  TEST_CASE("prepareWasapiRenderPacket pads short PCM without advancing silence", "[audio][regression][wasapi][render]")
  {
    auto buffer = std::array<std::byte, 16>{};
    std::ranges::fill(buffer, std::byte{0x5a});

    auto const packet = detail::prepareWasapiRenderPacket(
      buffer, 4, {.bytesWritten = 8, .positionFrameOffset = 1, .positionFrames = 1, .drained = false});

    CHECK(packet.renderedFrames == 2);
    CHECK(packet.framesToRelease == 4);
    CHECK(packet.underrun);
    CHECK_FALSE(packet.drained);

    for (std::size_t index = 0; index < buffer.size(); ++index)
    {
      CHECK(buffer[index] == (index < 8 ? std::byte{0x5a} : std::byte{0}));
    }
  }

  TEST_CASE("prepareWasapiRenderPacket releases drained short and empty packets correctly",
            "[audio][regression][wasapi][render]")
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
    CHECK(optFormat->bitDepth == 32);
    CHECK(optFormat->validBits == 24);
    CHECK_FALSE(optFormat->isFloat);
    CHECK(optFormat->isInterleaved);
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
} // namespace ao::audio::backend::test
