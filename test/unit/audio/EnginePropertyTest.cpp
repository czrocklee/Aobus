// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "BackendTestSupport.h"
#include "CapturingBackend.h"
#include "EngineTestSupport.h"
#include "ScriptedDecoderSession.h"
#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/Device.h>
#include <ao/audio/Engine.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Property.h>
#include <ao/audio/Transport.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

namespace ao::audio::test
{
  using namespace fakeit;

  TEST_CASE("Engine - volume and mute controls update backend and status", "[audio][unit][engine][property]")
  {
    auto spy = SpyBackend<>{};
    auto& mockBackend = spy.mock();
    auto const device = Device{.id = DeviceId{"test-device"},
                               .displayName = "Test",
                               .description = "Test",
                               .isDefault = false,
                               .backendId = kBackendNone};

    auto engine = Engine{spy.makeProxy(), device};
    auto lastSetPropertyId = PropertyId{};
    auto lastSetPropertyValue = PropertyValue{false};

    When(Method(mockBackend, setProperty))
      .AlwaysDo(
        [&](PropertyId id, PropertyValue const& value) -> Result<>
        {
          lastSetPropertyId = id;
          lastSetPropertyValue = value;
          return Result<>{};
        });

    CHECK(engine.setVolume(0.75F));
    CHECK(lastSetPropertyId == PropertyId::Volume);
    CHECK(std::get<float>(lastSetPropertyValue) == Catch::Approx{0.75F});
    CHECK(engine.volume() == Catch::Approx{0.75F});
    CHECK(engine.status().volume == Catch::Approx{0.75F});

    CHECK(engine.setMuted(true));
    CHECK(lastSetPropertyId == PropertyId::Muted);
    CHECK(std::get<bool>(lastSetPropertyValue) == true);
    CHECK(engine.isMuted() == true);
    CHECK(engine.status().muted == true);

    CHECK(engine.isVolumeAvailable() == true);
    CHECK(engine.status().volumeAvailable == true);
  }

  TEST_CASE("Engine - exposes property API", "[audio][unit][engine][property]")
  {
    auto const device = Device{.id = DeviceId{"test-device"},
                               .displayName = "Test",
                               .description = "Test",
                               .isDefault = false,
                               .backendId = kBackendNone};
    auto backendPtr = std::make_unique<CapturingBackend>();
    auto* backendRaw = backendPtr.get();

    auto const fmt = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isInterleaved = true};
    auto const factory = [fmt](auto const&, auto const&)
    {
      auto decPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
        .sourceFormat = fmt, .outputFormat = fmt, .duration = std::chrono::seconds{1}, .isLossy = false});
      decPtr->setReadScript({{.data = std::vector<std::byte>(88200, std::byte{0}), .endOfStream = false}});
      return decPtr;
    };

    auto engine = Engine{std::move(backendPtr), device, factory};
    auto const desc = PlaybackInput{.filePath = "test.flac"};

    SECTION("queryProperty returns all-false for unknown PropertyId")
    {
      auto constexpr kUnknownId = static_cast<PropertyId>(999);
      auto const info = backendRaw->queryProperty(kUnknownId);

      CHECK(info.canRead == false);
      CHECK(info.canWrite == false);
      CHECK(info.isAvailable == false);
      CHECK(info.emitsChangeNotifications == false);
    }

    SECTION("queryProperty returns valid info for Volume")
    {
      backendRaw->setMockPropertyInfo(PropertyId::Volume,
                                      PropertyInfo{
                                        .canRead = true,
                                        .canWrite = true,
                                        .isAvailable = true,
                                        .emitsChangeNotifications = false,
                                        .isHardwareAssisted = true,
                                      });

      auto const info = backendRaw->queryProperty(PropertyId::Volume);

      CHECK(info.canRead == true);
      CHECK(info.canWrite == true);
      CHECK(info.isAvailable == true);
      CHECK(info.isHardwareAssisted == true);
    }

    SECTION("setProperty returns error for unknown PropertyId")
    {
      auto constexpr kUnknownId = static_cast<PropertyId>(999);
      auto const result = backendRaw->setProperty(kUnknownId, PropertyValue{0.5F});

      REQUIRE(!result);
      CHECK(result.error().code == Error::Code::NotSupported);
    }

    SECTION("property returns error for unknown PropertyId")
    {
      auto constexpr kUnknownId = static_cast<PropertyId>(999);
      auto const result = backendRaw->property(kUnknownId);

      REQUIRE(!result);
      CHECK(result.error().code == Error::Code::NotSupported);
    }

    SECTION("onPropertyChanged callback updates engine volume status")
    {
      backendRaw->setMockPropertyInfo(PropertyId::Volume,
                                      PropertyInfo{
                                        .canRead = true,
                                        .canWrite = true,
                                        .isAvailable = true,
                                        .emitsChangeNotifications = false,
                                        .isHardwareAssisted = true,
                                      });

      // Play must be called so the backend target is initialized
      engine.play(makePlaybackItem(desc));

      auto stateChanged = CallbackLatch{};
      engine.setOnStateChanged([&] { stateChanged.notify(); });

      backendRaw->emitPropertyChanged(PropertyId::Volume);

      CHECK(stateChanged.waitForCount(1));
      CHECK(engine.status().volume == Catch::Approx{1.0F});
      CHECK(engine.volume() == Catch::Approx{1.0F});
      CHECK(engine.status().volumeAvailable == true);
      CHECK(engine.status().volumeIsHardwareAssisted == true);
    }

    SECTION("onPropertyChanged handles backend read errors gracefully")
    {
      backendRaw->setPropertyError(Error::Code::Generic);
      backendRaw->emitPropertyChanged(PropertyId::Volume);
      backendRaw->emitPropertyChanged(PropertyId::Muted);

      CHECK(engine.status().volumeAvailable);
      CHECK(engine.status().volume == Catch::Approx{1.0F});
      CHECK(engine.status().muted == false);
    }

    SECTION("onPropertyChanged callback updates engine mute status")
    {
      backendRaw->emitPropertyChanged(PropertyId::Muted);

      CHECK(engine.status().volumeAvailable);
      CHECK(engine.status().muted == false);
      CHECK(engine.isMuted() == false);
    }

    SECTION("onPropertyChanged callback for unknown property is ignored")
    {
      auto constexpr kUnknownId = static_cast<PropertyId>(999);
      backendRaw->emitPropertyChanged(kUnknownId);
    }

    SECTION("Backend callbacks update engine state correctly")
    {
      engine.play(makePlaybackItem(desc));
      auto stateChanged = CallbackLatch{};
      engine.setOnStateChanged([&] { stateChanged.notify(); });

      backendRaw->emitBackendError("hardware failed");
      CHECK(stateChanged.waitForCount(1));
      CHECK(engine.status().transport == Transport::Error);

      engine.play(makePlaybackItem(desc));
      auto routeChanged = CallbackLatch{};
      engine.setOnRouteChanged([&](auto const&) { routeChanged.notify(); });
      backendRaw->emitRouteReady("test-anchor");
      CHECK(routeChanged.waitForCount(1));
    }

    SECTION("setVolume round-trips through engine and backend")
    {
      CHECK(engine.setVolume(0.42F));
      CHECK(engine.volume() == Catch::Approx{0.42F});
      CHECK(engine.status().volume == Catch::Approx{0.42F});

      auto const backendVol = backendRaw->property(PropertyId::Volume);

      REQUIRE(backendVol);
      CHECK(std::get<float>(*backendVol) == Catch::Approx{0.42F});
    }

    SECTION("setMuted round-trips through engine and backend")
    {
      CHECK(engine.setMuted(true));
      CHECK(engine.isMuted() == true);
      CHECK(engine.status().muted == true);

      auto const backendMuted = backendRaw->property(PropertyId::Muted);

      REQUIRE(backendMuted);
      CHECK(std::get<bool>(*backendMuted) == true);
    }

    SECTION("property controls survive backend open")
    {
      CHECK(engine.setVolume(0.37F));
      CHECK(engine.setMuted(true));

      engine.play(makePlaybackItem(desc));

      CHECK(engine.status().volume == Catch::Approx{0.37F});
      CHECK(engine.status().muted == true);

      auto const backendVol = backendRaw->property(PropertyId::Volume);
      auto const backendMuted = backendRaw->property(PropertyId::Muted);

      REQUIRE(backendVol);
      REQUIRE(backendMuted);
      CHECK(std::get<float>(*backendVol) == Catch::Approx{0.37F});
      CHECK(std::get<bool>(*backendMuted) == true);
    }
  }
} // namespace ao::audio::test
