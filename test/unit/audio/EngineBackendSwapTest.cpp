// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EngineTestSupport.h"
#include "FakeCapturingBackend.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Engine.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Transport.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <utility>

namespace ao::audio::test
{
  TEST_CASE("Engine - setBackend with active track resumes playback", "[audio][unit][engine][hot-swap]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "test.flac"}));
    CHECK(engine.status().transport == Transport::Playing);

    auto newBackendPtr = std::make_unique<FakeCapturingBackend>();
    auto const newDevice = Device{.id = DeviceId{"new-device"},
                                  .displayName = "New",
                                  .description = "New",
                                  .isDefault = false,
                                  .backendId = kBackendNone};
    engine.setBackend(std::move(newBackendPtr), newDevice);

    auto const snap = engine.status();
    CHECK(snap.transport == Transport::Playing);
    CHECK(snap.currentDeviceId == "new-device");
  }
} // namespace ao::audio::test
