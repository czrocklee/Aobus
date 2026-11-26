// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EngineTestSupport.h"
#include "FakeCapturingBackend.h"
#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Engine.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/Transport.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <utility>

namespace ao::audio::test
{
  TEST_CASE("Engine - replacing an idle backend with the same IDs rejects a staged start")
  {
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto device = makeEngineTestDevice();
    device.backendId = backendPtr->backendId();
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};
    auto stagedRes = engine.stagePlayback(makePlaybackItem("staged.flac"));
    REQUIRE(stagedRes);

    auto replacementPtr = std::make_unique<FakeCapturingBackend>();
    auto* const replacement = replacementPtr.get();
    engine.setBackend(std::move(replacementPtr), device);
    auto const installedGeneration = engine.playbackGeneration();
    auto const commitRes = engine.commitPlayback(std::move(*stagedRes));

    REQUIRE_FALSE(commitRes);
    CHECK(commitRes.error().code == Error::Code::Conflict);
    CHECK(engine.playbackGeneration() == installedGeneration);
    CHECK(engine.transport() == Transport::Idle);
    CHECK(replacement->events().empty());
  }

  TEST_CASE("Engine - setBackend with active track resumes playback")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "test.flac"}));
    CHECK(engine.status().transport == Transport::Playing);

    auto newBackendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const newBackend = newBackendPtr.get();
    auto const newDevice = Device{.id = DeviceId{"new-device"},
                                  .displayName = "New",
                                  .description = "New",
                                  .isDefault = false,
                                  .backendId = kBackendNone};
    engine.setBackend(std::move(newBackendPtr), newDevice);

    auto const snap = engine.status();
    CHECK(snap.transport == Transport::Playing);
    CHECK(snap.currentDeviceId == "new-device");
    auto const events = newBackend->events();
    REQUIRE(events.size() == 2);
    CHECK(events[0].name == "open");
    CHECK(events[0].format == makeEngineTestFormat());
    CHECK(events[1].name == "start");

    auto* const target = newBackend->target();
    REQUIRE(target != nullptr);
    auto output = std::array<std::byte, 8>{};
    output.fill(std::byte{0xEE});
    REQUIRE(target->renderPcm(output).bytesWritten == output.size());
    CHECK(output == std::array<std::byte, 8>{});
  }
} // namespace ao::audio::test
