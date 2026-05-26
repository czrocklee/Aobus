// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/OutputSelectorState.h"

#include <ao/audio/Backend.h>
#include <ao/rt/StateTypes.h>

#include <catch2/catch_test_macros.hpp>

using namespace ao;
using namespace ao::gtk;

TEST_CASE("buildOutputSelectorRows - mapping and active state", "[gtk][playback]")
{
  auto state = rt::PlaybackState{};
  state.selectedOutput = rt::OutputSelection{.backendId = audio::BackendId{"alsa"},
                                             .deviceId = audio::DeviceId{"hw:0,0"},
                                             .profileId = audio::kProfileExclusive};

  auto backend = rt::OutputBackendSnapshot{};
  backend.id = audio::BackendId{"alsa"};
  backend.name = "ALSA";

  auto device = rt::OutputDeviceSnapshot{};
  device.id = audio::DeviceId{"hw:0,0"};
  device.displayName = "Built-in Audio";

  backend.devices.push_back(device);
  backend.supportedProfiles.push_back({audio::kProfileShared, "Shared"});
  backend.supportedProfiles.push_back({audio::kProfileExclusive, "Exclusive"});

  state.availableOutputs.push_back(backend);

  auto rows = buildOutputSelectorRows(state);

  REQUIRE(rows.size() == 3);

  SECTION("Header is emitted first")
  {
    CHECK(rows[0].kind == OutputSelectorRow::Kind::BackendHeader);
    CHECK(rows[0].title == "ALSA");
    CHECK(rows[0].backendId == audio::BackendId{"alsa"});
  }

  SECTION("Default profile device is emitted")
  {
    CHECK(rows[1].kind == OutputSelectorRow::Kind::DeviceProfile);
    CHECK(rows[1].title == "Built-in Audio");
    CHECK(rows[1].backendId == audio::BackendId{"alsa"});
    CHECK(rows[1].deviceId == audio::DeviceId{"hw:0,0"});
    CHECK(rows[1].profileId == audio::kProfileShared);
    CHECK(rows[1].active == false);
  }

  SECTION("Exclusive profile device has [E] suffix and is marked active")
  {
    CHECK(rows[2].kind == OutputSelectorRow::Kind::DeviceProfile);
    CHECK(rows[2].title == "Built-in Audio [E]");
    CHECK(rows[2].backendId == audio::BackendId{"alsa"});
    CHECK(rows[2].deviceId == audio::DeviceId{"hw:0,0"});
    CHECK(rows[2].profileId == audio::kProfileExclusive);
    CHECK(rows[2].active == true);
  }
}
