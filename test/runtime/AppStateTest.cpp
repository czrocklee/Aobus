// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/rt/AppState.h>

#include "test/TestFixtureSupport.h"
#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/rt/ConfigStore.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <string>

namespace ao::rt::test
{
  namespace
  {
    audio::OutputDeviceSelection makeSelection(std::string const& device,
                                               audio::OutputRoutingPolicy policy = audio::OutputRoutingPolicy::Fixed)
    {
      return {
        .backendId = audio::kBackendPipeWire,
        .deviceId = audio::DeviceId{device},
        .profileId = audio::kProfileExclusive,
        .routingPolicy = policy,
      };
    }

    void writeConfig(std::filesystem::path const& path, std::string const& contents)
    {
      auto stream = std::ofstream{path, std::ios::binary};
      stream << contents;
    }
  } // namespace

  TEST_CASE("AppState - a named PipeWire preference persists its rerouting policy")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";
    auto store = ConfigStore{configPath};
    auto written = AppPrefsState{
      .preferredOutputSelection = makeSelection("node:studio-dac", audio::OutputRoutingPolicy::AllowSystemReroute),
      .lastLayoutPreset = "compact",
      .lastThemePreset = "midnight",
    };
    REQUIRE(saveAppPrefs(store, written));
    auto read = AppPrefsState{};

    SECTION("opt-in round trips")
    {
      CHECK(ao::test::readFile(configPath).contains("allowSystemOutputReroute: true"));
    }

    SECTION("explicit off replaces opt-in")
    {
      read = written;
      written.preferredOutputSelection.routingPolicy = audio::OutputRoutingPolicy::Fixed;
      REQUIRE(saveAppPrefs(store, written));
      CHECK(ao::test::readFile(configPath).contains("allowSystemOutputReroute: false"));
    }

    auto reloaded = ConfigStore{configPath};
    loadAppPrefs(reloaded, read);

    CHECK(read.preferredOutputSelection == written.preferredOutputSelection);
    CHECK(read.lastLayoutPreset == "compact");
    CHECK(read.lastThemePreset == "midnight");
  }

  TEST_CASE("AppState - unsupported output routes cannot load or save rerouting permission")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";
    auto selection = makeSelection("node:studio-dac", audio::OutputRoutingPolicy::AllowSystemReroute);

    SECTION("non-PipeWire backend")
    {
      selection.backendId = audio::kBackendAlsa;
    }

    SECTION("unnamed PipeWire default")
    {
      selection.deviceId = {};
      selection.profileId = audio::kProfileShared;
    }

    SECTION("raw numeric PipeWire node id")
    {
      selection.deviceId = audio::DeviceId{"42"};
    }

    SECTION("empty PipeWire node name")
    {
      selection.deviceId = audio::DeviceId{"node:"};
    }

    writeConfig(configPath,
                std::format("runtime:\n"
                            "  version: 1\n"
                            "  lastOutputBackendId: {}\n"
                            "  lastOutputProfileId: {}\n"
                            "  lastOutputDeviceId: \"{}\"\n"
                            "  allowSystemOutputReroute: true\n",
                            selection.backendId.raw(),
                            selection.profileId.raw(),
                            selection.deviceId.raw()));
    auto store = ConfigStore{configPath};
    auto prefs = AppPrefsState{};
    loadAppPrefs(store, prefs);

    auto expected = selection;
    expected.routingPolicy = audio::OutputRoutingPolicy::Fixed;
    CHECK(prefs.preferredOutputSelection == expected);

    prefs.preferredOutputSelection = selection;
    REQUIRE(saveAppPrefs(store, prefs));
    CHECK_FALSE(ao::test::readFile(configPath).contains("allowSystemOutputReroute"));
  }

  TEST_CASE("AppState - last active output saves only identity without changing the preferred route")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";
    auto store = ConfigStore{configPath};
    auto const prefs = AppPrefsState{
      .preferredOutputSelection = makeSelection("node:studio-dac", audio::OutputRoutingPolicy::AllowSystemReroute),
      .lastLayoutPreset = {},
      .lastThemePreset = {},
    };
    REQUIRE(saveAppPrefs(store, prefs));
    auto const written = AppSessionState{
      .lastLibraryPath = "/music/library",
      .lastOutputSelection = makeSelection("node:headphones", audio::OutputRoutingPolicy::AllowSystemReroute),
    };
    REQUIRE(saveAppSession(store, written));

    auto reloaded = ConfigStore{configPath};
    auto read = AppSessionState{};
    read.lastOutputSelection = makeSelection("node:seeded-headphones", audio::OutputRoutingPolicy::AllowSystemReroute);
    loadAppSession(reloaded, read);

    CHECK(read.lastLibraryPath == "/music/library");
    CHECK(read.lastOutputSelection == makeSelection("node:headphones"));
    auto loadedPrefs = AppPrefsState{};
    loadAppPrefs(reloaded, loadedPrefs);
    CHECK(loadedPrefs.preferredOutputSelection == prefs.preferredOutputSelection);

    // With preferences removed, the session must not contain another copy of the permission.
    REQUIRE(reloaded.removeGroup(kAppPrefsConfigGroup));
    CHECK_FALSE(ao::test::readFile(configPath).contains("allowSystemOutputReroute"));
  }

  TEST_CASE("AppState - absent groups retain caller defaults except for output rerouting permission")
  {
    auto const tempDir = ao::test::TempDir{};
    auto store = ConfigStore{tempDir.path() / "config.yaml"};

    auto prefs = AppPrefsState{
      .preferredOutputSelection =
        makeSelection("node:seeded-preference-device", audio::OutputRoutingPolicy::AllowSystemReroute),
      .lastLayoutPreset = "seeded-layout",
      .lastThemePreset = "seeded-theme",
    };
    loadAppPrefs(store, prefs);
    CHECK(prefs.preferredOutputSelection == makeSelection("node:seeded-preference-device"));
    CHECK(prefs.lastLayoutPreset == "seeded-layout");
    CHECK(prefs.lastThemePreset == "seeded-theme");

    auto session = AppSessionState{
      .lastLibraryPath = "/seeded/library",
      .lastOutputSelection =
        makeSelection("node:seeded-session-device", audio::OutputRoutingPolicy::AllowSystemReroute),
    };
    loadAppSession(store, session);
    CHECK(session.lastLibraryPath == "/seeded/library");
    CHECK(session.lastOutputSelection == makeSelection("node:seeded-session-device"));
  }

  TEST_CASE("AppState - a document written by another build stays readable")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";

    SECTION("legacy preferences retain missing preset defaults but not seeded routing permission")
    {
      writeConfig(configPath, R"(runtime:
  version: 1
  lastOutputBackendId: pipewire
  lastOutputProfileId: exclusive
  lastOutputDeviceId: node:studio-dac
)");
      auto store = ConfigStore{configPath};
      auto state = AppPrefsState{
        .preferredOutputSelection = makeSelection("node:seeded-dac", audio::OutputRoutingPolicy::AllowSystemReroute),
        .lastLayoutPreset = "seeded-layout",
        .lastThemePreset = "seeded-theme",
      };
      loadAppPrefs(store, state);

      CHECK(state.preferredOutputSelection == makeSelection("node:studio-dac"));
      CHECK(state.lastLayoutPreset == "seeded-layout");
      CHECK(state.lastThemePreset == "seeded-theme");
    }

    SECTION("unknown session fields are ignored even if they name a known preference")
    {
      writeConfig(configPath, R"(session:
  lastLibraryPath: /music
  lastOutputBackendId: pipewire
  lastOutputProfileId: exclusive
  lastOutputDeviceId: node:headphones
  allowSystemOutputReroute: true
  somethingFromTheFuture: 12
)");
      auto store = ConfigStore{configPath};
      auto state = AppSessionState{};
      loadAppSession(store, state);

      CHECK(state.lastLibraryPath == "/music");
      CHECK(state.lastOutputSelection == makeSelection("node:headphones"));
    }
  }

  TEST_CASE("AppState - an incomplete stored route cannot grant rerouting permission")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";
    auto fields = std::string{};

    SECTION("missing device")
    {
      fields = "  lastOutputBackendId: pipewire\n  lastOutputProfileId: exclusive\n";
    }

    SECTION("missing profile")
    {
      fields = "  lastOutputBackendId: pipewire\n  lastOutputDeviceId: node:seeded-dac\n";
    }

    writeConfig(configPath, "runtime:\n  version: 1\n  allowSystemOutputReroute: true\n" + fields);
    auto store = ConfigStore{configPath};
    auto prefs = AppPrefsState{};
    prefs.preferredOutputSelection = makeSelection("node:seeded-dac");
    loadAppPrefs(store, prefs);

    CHECK(prefs.preferredOutputSelection == makeSelection("node:seeded-dac"));
  }

  TEST_CASE("AppState - malformed permission keeps the explicitly preferred route fixed")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";
    auto prefs = AppPrefsState{};
    auto value = std::string{};

    SECTION("unknown boolean spelling with no seeded route")
    {
      value = "yes";
    }

    SECTION("a quoted string does not opt in over a permitted seed")
    {
      value = "\"true\"";
      prefs.preferredOutputSelection = makeSelection("node:seeded-dac", audio::OutputRoutingPolicy::AllowSystemReroute);
    }

    writeConfig(configPath,
                R"(runtime:
  version: 1
  lastOutputBackendId: pipewire
  lastOutputProfileId: exclusive
  lastOutputDeviceId: node:studio-dac
  lastLayoutPreset: compact
  allowSystemOutputReroute: )" +
                  value + '\n');
    auto store = ConfigStore{configPath};
    loadAppPrefs(store, prefs);

    CHECK(prefs.preferredOutputSelection == makeSelection("node:studio-dac"));
    CHECK(prefs.lastLayoutPreset == "compact");
  }

  TEST_CASE("AppState - a malformed known identity field rejects only its own group")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";
    writeConfig(configPath, R"(runtime:
  version: 1
  lastOutputBackendId: [not, a, scalar]
  allowSystemOutputReroute: true
session:
  lastLibraryPath: /music
)");
    auto store = ConfigStore{configPath};

    auto prefs = AppPrefsState{
      .preferredOutputSelection =
        makeSelection("node:seeded-preference-device", audio::OutputRoutingPolicy::AllowSystemReroute),
      .lastLayoutPreset = "seeded-layout",
      .lastThemePreset = "seeded-theme",
    };
    loadAppPrefs(store, prefs);
    CHECK(prefs.preferredOutputSelection == makeSelection("node:seeded-preference-device"));
    CHECK(prefs.lastLayoutPreset == "seeded-layout");
    CHECK(prefs.lastThemePreset == "seeded-theme");

    auto session = AppSessionState{
      .lastLibraryPath = "/seeded/library",
      .lastOutputSelection = makeSelection("node:seeded-session-device"),
    };
    auto const sessionOutputBefore = session.lastOutputSelection;
    loadAppSession(store, session);
    CHECK(session.lastLibraryPath == "/music");
    CHECK(session.lastOutputSelection == sessionOutputBefore);
  }

  TEST_CASE("AppState - a preferences group of a newer version is kept and never overwritten")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";
    auto const document = std::string{R"(runtime:
  version: 2
  lastOutputDeviceId: node:studio-dac
  fromTheFuture: 12
)"};
    writeConfig(configPath, document);
    auto store = ConfigStore{configPath};

    auto prefs = AppPrefsState{.lastLayoutPreset = "", .lastThemePreset = "seeded-theme"};
    loadAppPrefs(store, prefs);
    CHECK(prefs.preferredOutputSelection == audio::OutputDeviceSelection{});
    CHECK(prefs.lastThemePreset == "seeded-theme");
    CHECK(store.hasNewerGroup(kAppPrefsConfigGroup));

    auto const savedRes = saveAppPrefs(store, prefs);
    REQUIRE_FALSE(savedRes);
    CHECK(savedRes.error().code == Error::Code::NotSupported);
    CHECK(ao::test::readFile(configPath) == document);

    REQUIRE(saveAppSession(store, AppSessionState{.lastLibraryPath = "/music"}));
    CHECK(ao::test::readFile(configPath).contains(document));
  }

  TEST_CASE("AppState - a preferences group without a version keeps the seeded state")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";
    writeConfig(configPath, "runtime:\n  lastThemePreset: midnight\n");
    auto store = ConfigStore{configPath};

    auto prefs = AppPrefsState{.lastLayoutPreset = "", .lastThemePreset = "seeded-theme"};
    loadAppPrefs(store, prefs);
    CHECK(prefs.lastThemePreset == "seeded-theme");
    CHECK_FALSE(store.hasNewerGroup(kAppPrefsConfigGroup));
  }
} // namespace ao::rt::test
