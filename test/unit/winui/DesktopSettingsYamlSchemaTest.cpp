// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/DesktopSettingsYamlSchema.h>

#include "test/unit/TestFixtureSupport.h"
#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/rt/ConfigStore.h>
#include <ao/winui/app/DesktopOutputSelection.h>
#include <ao/winui/layout/ShellState.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <format>
#include <string>
#include <utility>

namespace ao::winui::test
{
  TEST_CASE("DesktopSettingsYamlSchema - round-trip owns independent desktop state", "[winui][unit][layout]")
  {
    auto state = DesktopSettings{};
    state.window = {.x = 120, .y = 140, .width = 1440, .height = 900, .maximized = true};
    state.shellMode = ShellMode::Classic;
    state.lastLibraryPath = "C:/Music";
    state.preferredOutputSelection = {
      .backendId = audio::kBackendWasapi,
      .deviceId = audio::DeviceId{"studio-dac"},
      .profileId = audio::kProfileExclusive,
    };
    state.navigationPaneWidth = 260.0;
    state.inspectorPaneWidth = 360.0;

    auto tree = ryml::Tree{yaml::callbacks()};
    REQUIRE(DesktopSettingsYamlSchema{}.serialize(tree.rootref(), state));
    auto decodedRes = DesktopSettingsYamlSchema{}.deserialize(tree.rootref(), DesktopSettings{});

    REQUIRE(decodedRes);
    CHECK(*decodedRes == state);
  }

  TEST_CASE("DesktopSettingsYamlSchema - accepts pane width boundaries", "[winui][unit][layout]")
  {
    auto state = DesktopSettings{};
    state.navigationPaneWidth = kMaximumNavigationPaneWidth;
    state.inspectorPaneWidth = kMinimumInspectorPaneWidth;

    auto tree = ryml::Tree{yaml::callbacks()};
    REQUIRE(DesktopSettingsYamlSchema{}.serialize(tree.rootref(), state));
    auto decodedRes = DesktopSettingsYamlSchema{}.deserialize(tree.rootref(), DesktopSettings{});

    REQUIRE(decodedRes);
    CHECK(decodedRes->navigationPaneWidth == kMaximumNavigationPaneWidth);
    CHECK(decodedRes->inspectorPaneWidth == kMinimumInspectorPaneWidth);
  }

  TEST_CASE("DesktopSettings - captured normal bounds preserve position and unrelated preferences",
            "[winui][unit][layout]")
  {
    for (auto const& [captured, expectedWindow] : {
           std::pair{WindowPlacement{.x = -21, .y = 37, .width = 639, .height = 700},
                     WindowPlacement{.x = -21, .y = 37, .width = 640, .height = 700}},
           std::pair{WindowPlacement{.x = 40, .y = -30, .width = 900, .height = 479},
                     WindowPlacement{.x = 40, .y = -30, .width = 900, .height = 480}},
           std::pair{WindowPlacement{.x = 41, .y = 42, .width = 639, .height = 479, .maximized = true},
                     WindowPlacement{.x = 41, .y = 42, .width = 640, .height = 480, .maximized = true}},
           std::pair{WindowPlacement{.x = -81, .y = -82, .width = 640, .height = 480},
                     WindowPlacement{.x = -81, .y = -82, .width = 640, .height = 480}},
           std::pair{WindowPlacement{.x = -41, .y = 42, .width = 1440, .height = 900, .maximized = true},
                     WindowPlacement{.x = -41, .y = 42, .width = 1440, .height = 900, .maximized = true}},
         })
    {
      INFO("captured " << captured.width << "x" << captured.height);
      auto state = DesktopSettings{};
      state.shellMode = ShellMode::Classic;
      state.lastLibraryPath = "C:/Music";
      state.preferredOutputSelection = {
        .backendId = audio::kBackendWasapi,
        .deviceId = audio::DeviceId{"studio-dac"},
        .profileId = audio::kProfileExclusive,
      };
      state.navigationPaneWidth = 275.0;
      state.inspectorPaneWidth = 375.0;
      auto expected = state;
      expected.window = expectedWindow;

      rememberDesktopWindowPlacement(state, captured);

      CHECK(state == expected);
    }
  }

  TEST_CASE("DesktopSettings - small window checkpoints preserve later edits and valid recapture",
            "[winui][unit][layout]")
  {
    auto const fixture = ao::test::TempDir{};
    auto const configPath = fixture.path() / "windows-settings.yaml";
    auto store = rt::ConfigStore{configPath};
    auto state = DesktopSettings{};
    state.lastLibraryPath = "C:/Music";
    auto const schema = DesktopSettingsYamlSchema{};
    REQUIRE(store.save("desktop", state, schema));

    auto const checkpoint = [&]
    {
      REQUIRE(store.save("desktop", state, schema));
      auto freshStore = rt::ConfigStore{configPath};
      auto reloaded = DesktopSettings{};
      auto const loadedRes = freshStore.load("desktop", reloaded, schema);
      REQUIRE(loadedRes);
      REQUIRE(*loadedRes);
      CHECK(reloaded == state);
    };

    // These are normal bounds retained while maximized, not the maximized extent.
    rememberDesktopWindowPlacement(state, {.x = -45, .y = 61, .width = 639, .height = 479, .maximized = true});
    CHECK(state.window == WindowPlacement{.x = -45, .y = 61, .width = 640, .height = 480, .maximized = true});
    checkpoint();

    // Exercise the captured state reused by later checkpoints, not native UI routing.
    state.shellMode = ShellMode::Classic;
    checkpoint();
    state.navigationPaneWidth = 275.0;
    state.inspectorPaneWidth = 375.0;
    checkpoint();
    auto const selection = audio::OutputDeviceSelection{
      .backendId = audio::kBackendWasapi,
      .deviceId = audio::DeviceId{"headphones"},
      .profileId = audio::kProfileShared,
    };
    REQUIRE(tryRememberDesktopOutputSelection(state, selection));
    CHECK(state.preferredOutputSelection == selection);
    checkpoint();

    auto expected = state;
    expected.window = {.x = 131, .y = -71, .width = 1440, .height = 900, .maximized = false};
    rememberDesktopWindowPlacement(state, expected.window);
    CHECK(state == expected);
    checkpoint();
  }

  TEST_CASE("DesktopSettingsYamlSchema - undersized windows cannot replace a durable valid setting",
            "[winui][unit][layout]")
  {
    auto const fixture = ao::test::TempDir{};
    auto const configPath = fixture.path() / "windows-settings.yaml";
    auto store = rt::ConfigStore{configPath};
    auto valid = DesktopSettings{};
    valid.window = {.x = 41, .y = 42, .width = 640, .height = 480, .maximized = true};
    valid.shellMode = ShellMode::Classic;
    valid.lastLibraryPath = "C:/previous-library";
    valid.navigationPaneWidth = 275.0;
    valid.inspectorPaneWidth = 375.0;
    auto const schema = DesktopSettingsYamlSchema{};

    auto controlTree = ryml::Tree{yaml::callbacks()};
    REQUIRE(schema.serialize(controlTree.rootref(), valid));
    auto const controlRes = schema.deserialize(controlTree.rootref(), DesktopSettings{});
    REQUIRE(controlRes);
    CHECK(*controlRes == valid);

    REQUIRE(store.save("desktop", valid, schema));
    auto const originalBytes = ao::test::readFile(configPath);
    auto originalStore = rt::ConfigStore{configPath};
    auto original = DesktopSettings{};
    auto const originalRes = originalStore.load("desktop", original, schema);
    REQUIRE(originalRes);
    REQUIRE(*originalRes);
    CHECK(original == valid);

    for (auto const invalidWindow :
         {WindowPlacement{.width = 639, .height = 480}, WindowPlacement{.width = 640, .height = 479}})
    {
      INFO("window " << invalidWindow.width << "x" << invalidWindow.height);
      auto invalid = valid;
      invalid.window.width = invalidWindow.width;
      invalid.window.height = invalidWindow.height;

      auto tree = ryml::Tree{yaml::callbacks()};
      auto const serializedRes = schema.serialize(tree.rootref(), invalid);
      REQUIRE_FALSE(serializedRes);
      CHECK(serializedRes.error().code == Error::Code::FormatRejected);
      CHECK(serializedRes.error().message == "Windows window size must be at least 640x480");

      auto const source = std::format("version: {}\nwindow: {{width: {}, height: {}}}\n",
                                      kDesktopSettingsVersion,
                                      invalidWindow.width,
                                      invalidWindow.height);
      auto parsed = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &parsed);
      auto const decodedRes = schema.deserialize(parsed.rootref(), valid);
      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message == "Windows window size must be at least 640x480");

      auto const savedRes = store.save("desktop", invalid, schema);
      REQUIRE_FALSE(savedRes);
      CHECK(savedRes.error().code == Error::Code::FormatRejected);
      CHECK(savedRes.error().message ==
            "Failed to serialize config group 'desktop': Windows window size must be at least 640x480");
      CHECK(ao::test::readFile(configPath) == originalBytes);

      auto reloadedStore = rt::ConfigStore{configPath};
      auto reloaded = DesktopSettings{};
      auto const loadedRes = reloadedStore.load("desktop", reloaded, schema);
      REQUIRE(loadedRes);
      REQUIRE(*loadedRes);
      CHECK(reloaded == valid);
    }
  }

  TEST_CASE("DesktopSettingsYamlSchema - reads a version 2 document and upgrades it", "[winui][unit][layout]")
  {
    auto const* source = R"(
version: 2
window: {x: 17, y: 29, width: 1500, height: 950, maximized: true}
shellMode: classic
lastLibraryPath: 'C:/Legacy Music'
navigationPaneWidth: 271
inspectorPaneWidth: 371
)";
    auto tree = ryml::Tree{yaml::callbacks()};
    ryml::parse_in_arena(ryml::to_csubstr(source), &tree);

    auto const res = DesktopSettingsYamlSchema{}.deserialize(tree.rootref(), DesktopSettings{});

    REQUIRE(res);
    CHECK(res->version == kDesktopSettingsVersion);
    CHECK(res->window == WindowPlacement{.x = 17, .y = 29, .width = 1500, .height = 950, .maximized = true});
    CHECK(res->shellMode == ShellMode::Classic);
    CHECK(res->lastLibraryPath == "C:/Legacy Music");
    CHECK(res->navigationPaneWidth == 271.0);
    CHECK(res->inspectorPaneWidth == 371.0);
    CHECK(res->preferredOutputSelection == audio::OutputDeviceSelection{});
  }

  TEST_CASE("DesktopSettingsYamlSchema - refuses a newer document rather than truncating it", "[winui][unit][layout]")
  {
    auto const source = std::format(R"(
version: {}
shellMode: modern
)",
                                    kDesktopSettingsVersion + 1);
    auto tree = ryml::Tree{yaml::callbacks()};
    ryml::parse_in_arena(ryml::to_csubstr(source), &tree);

    auto const res = DesktopSettingsYamlSchema{}.deserialize(tree.rootref(), DesktopSettings{});

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::NotSupported);
  }

  TEST_CASE("DesktopSettingsYamlSchema - accepts exactly the versions that were written", "[winui][unit][layout]")
  {
    // The schema shipped at version 2, so 0 and 1 name no document that ever
    // existed. Zero in particular is what a missing or malformed marker parses
    // to, and reading one under current field semantics would dress a corrupt
    // document up as an old one.
    auto const deserialized = [](std::uint32_t const version)
    {
      auto const source = std::format("version: {}\nshellMode: modern\n", version);
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      return DesktopSettingsYamlSchema{}.deserialize(tree.rootref(), DesktopSettings{});
    };

    for (auto const rejected : {std::uint32_t{0}, std::uint32_t{1}, kDesktopSettingsVersion + 1})
    {
      INFO("version " << rejected);
      auto const res = deserialized(rejected);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotSupported);
    }

    for (auto const accepted : {std::uint32_t{2}, kDesktopSettingsVersion})
    {
      INFO("version " << accepted);
      auto const res = deserialized(accepted);
      REQUIRE(res);
      CHECK(res->version == kDesktopSettingsVersion);
    }
  }

  TEST_CASE("DesktopSettingsYamlSchema - an absent field keeps the seeded value", "[winui][unit][layout]")
  {
    auto seed = DesktopSettings{};
    seed.window = {.x = 5, .y = 6, .width = 1300, .height = 810, .maximized = false};
    seed.lastLibraryPath = "C:/Seeded";
    seed.preferredOutputSelection = {
      .backendId = audio::kBackendWasapi,
      .deviceId = audio::DeviceId{"seed-dac"},
      .profileId = audio::kProfileShared,
    };
    seed.navigationPaneWidth = 250.0;
    seed.inspectorPaneWidth = 330.0;

    SECTION("every optional field absent")
    {
      auto const* source = R"(
version: 3
shellMode: classic
)";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);

      auto const res = DesktopSettingsYamlSchema{}.deserialize(tree.rootref(), seed);

      REQUIRE(res);
      CHECK(res->shellMode == ShellMode::Classic);
      CHECK(res->window == seed.window);
      CHECK(res->lastLibraryPath == "C:/Seeded");
      CHECK(res->preferredOutputSelection == seed.preferredOutputSelection);
      CHECK(res->navigationPaneWidth == 250.0);
      CHECK(res->inspectorPaneWidth == 330.0);
    }

    SECTION("partial window placement")
    {
      auto const* source = R"(
version: 3
window: {maximized: true}
)";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);

      auto const res = DesktopSettingsYamlSchema{}.deserialize(tree.rootref(), seed);

      REQUIRE(res);
      CHECK(res->window == WindowPlacement{.x = 5, .y = 6, .width = 1300, .height = 810, .maximized = true});
      CHECK(res->shellMode == seed.shellMode);
    }
  }

  TEST_CASE("DesktopSettingsYamlSchema - rejects noncanonical persisted state", "[winui][unit][layout]")
  {
    SECTION("unknown shell mode")
    {
      auto const* source = R"(
version: 3
window: {x: 0, y: 0, width: 1280, height: 800, maximized: false}
shellMode: future
lastLibraryPath: ''
lastOutputBackendId: ''
lastOutputProfileId: ''
lastOutputDeviceId: ''
navigationPaneWidth: 240
inspectorPaneWidth: 320
)";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto res = DesktopSettingsYamlSchema{}.deserialize(tree.rootref(), DesktopSettings{});

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
      CHECK(res.error().message.contains("shell mode"));
    }

    SECTION("unknown token")
    {
      auto const* source = R"(
version: 3
window: {x: 0, y: 0, width: 1280, height: 800, maximized: false}
shellMode: modern
lastLibraryPath: ''
lastOutputBackendId: ''
lastOutputProfileId: ''
lastOutputDeviceId: ''
navigationPaneWidth: 240
inspectorPaneWidth: 320
future: true
)";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto res = DesktopSettingsYamlSchema{}.deserialize(tree.rootref(), DesktopSettings{});

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
      CHECK(res.error().message.contains("future"));
    }

    SECTION("noncurrent version cannot be serialized")
    {
      auto state = DesktopSettings{};
      state.version = kDesktopSettingsVersion - 1;
      auto tree = ryml::Tree{yaml::callbacks()};

      auto res = DesktopSettingsYamlSchema{}.serialize(tree.rootref(), state);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotSupported);
    }

    SECTION("oversized navigation pane")
    {
      auto state = DesktopSettings{};
      state.navigationPaneWidth = kMaximumNavigationPaneWidth + 1.0;
      auto tree = ryml::Tree{yaml::callbacks()};

      auto res = DesktopSettingsYamlSchema{}.serialize(tree.rootref(), state);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
      CHECK(res.error().message.contains("pane widths"));
    }

    SECTION("oversized inspector pane")
    {
      auto const* source = R"(
version: 3
window: {x: 0, y: 0, width: 1280, height: 800, maximized: false}
shellMode: modern
lastLibraryPath: ''
lastOutputBackendId: ''
lastOutputProfileId: ''
lastOutputDeviceId: ''
navigationPaneWidth: 240
inspectorPaneWidth: 481
)";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto res = DesktopSettingsYamlSchema{}.deserialize(tree.rootref(), DesktopSettings{});

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
      CHECK(res.error().message.contains("pane widths"));
    }
  }
} // namespace ao::winui::test
