// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/rt/AppStateStore.h>

#include "test/unit/TestFixtureSupport.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/rt/AppPrefsState.h>
#include <ao/rt/ConfigStore.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace ao::rt::test
{
  namespace
  {
    audio::OutputDeviceSelection makeSelection(std::string const& device)
    {
      return {
        .backendId = audio::kBackendPipeWire,
        .deviceId = audio::DeviceId{device},
        .profileId = audio::kProfileExclusive,
      };
    }

    void writeConfig(std::filesystem::path const& path, std::string const& contents)
    {
      auto stream = std::ofstream{path};
      stream << contents;
    }
  } // namespace

  TEST_CASE("AppStateStore - application state round-trips without a frontend", "[runtime][unit][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";

    SECTION("preferences")
    {
      auto store = ConfigStore{configPath};
      auto written = AppPrefsState{
        .preferredOutputSelection = makeSelection("studio-dac"),
        .lastLayoutPreset = "compact",
        .lastThemePreset = "midnight",
      };
      REQUIRE(saveAppPrefs(store, written));

      auto reloaded = ConfigStore{configPath};
      auto read = AppPrefsState{};
      loadAppPrefs(reloaded, read);

      CHECK(read.preferredOutputSelection == written.preferredOutputSelection);
      CHECK(read.lastLayoutPreset == "compact");
      CHECK(read.lastThemePreset == "midnight");
    }

    SECTION("session")
    {
      auto store = ConfigStore{configPath};
      auto written = AppSessionState{
        .lastLibraryPath = "/music/library",
        .lastOutputSelection = makeSelection("headphones"),
      };
      REQUIRE(saveAppSession(store, written));

      auto reloaded = ConfigStore{configPath};
      auto read = AppSessionState{};
      loadAppSession(reloaded, read);

      CHECK(read.lastLibraryPath == "/music/library");
      CHECK(read.lastOutputSelection == written.lastOutputSelection);
    }
  }

  TEST_CASE("AppStateStore - an absent group leaves the caller's state untouched", "[runtime][unit][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto store = ConfigStore{tempDir.path() / "config.yaml"};

    auto prefs = AppPrefsState{.lastLayoutPreset = "", .lastThemePreset = "seeded"};
    loadAppPrefs(store, prefs);
    CHECK(prefs.lastThemePreset == "seeded");

    auto session = AppSessionState{.lastLibraryPath = "/seeded"};
    loadAppSession(store, session);
    CHECK(session.lastLibraryPath == "/seeded");
  }

  TEST_CASE("AppStateStore - a document written by another build stays readable", "[runtime][unit][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";

    SECTION("fields the document predates keep the seeded value")
    {
      // The group as an older build wrote it: no theme or layout preset yet.
      writeConfig(configPath, R"(runtime:
  lastOutputBackendId: pipewire
  lastOutputProfileId: exclusive
  lastOutputDeviceId: studio-dac
)");
      auto store = ConfigStore{configPath};
      auto state = AppPrefsState{.lastLayoutPreset = "seeded-layout", .lastThemePreset = "seeded-theme"};
      loadAppPrefs(store, state);

      CHECK(state.preferredOutputSelection == makeSelection("studio-dac"));
      CHECK(state.lastLayoutPreset == "seeded-layout");
      CHECK(state.lastThemePreset == "seeded-theme");
    }

    SECTION("fields a newer build added are ignored rather than rejected")
    {
      writeConfig(configPath, R"(session:
  lastLibraryPath: /music
  somethingFromTheFuture: 12
)");
      auto store = ConfigStore{configPath};
      auto state = AppSessionState{};
      loadAppSession(store, state);

      CHECK(state.lastLibraryPath == "/music");
    }
  }

  TEST_CASE("AppStateStore - a malformed known field rejects only its own group", "[runtime][unit][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";
    writeConfig(configPath, R"(runtime:
  lastOutputBackendId: [not, a, scalar]
session:
  lastLibraryPath: /music
)");
    auto store = ConfigStore{configPath};

    auto prefs = AppPrefsState{.lastLayoutPreset = "", .lastThemePreset = "seeded"};
    loadAppPrefs(store, prefs);
    CHECK(prefs.lastThemePreset == "seeded");

    auto session = AppSessionState{};
    loadAppSession(store, session);
    CHECK(session.lastLibraryPath == "/music");
  }
} // namespace ao::rt::test
