// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/DesktopSettingsYamlSchema.h>

#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/winui/layout/ShellState.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <format>
#include <string>

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

    auto const result = DesktopSettingsYamlSchema{}.deserialize(tree.rootref(), DesktopSettings{});

    REQUIRE(result);
    CHECK(result->version == kDesktopSettingsVersion);
    CHECK(result->window == WindowPlacement{.x = 17, .y = 29, .width = 1500, .height = 950, .maximized = true});
    CHECK(result->shellMode == ShellMode::Classic);
    CHECK(result->lastLibraryPath == "C:/Legacy Music");
    CHECK(result->navigationPaneWidth == 271.0);
    CHECK(result->inspectorPaneWidth == 371.0);
    CHECK(result->preferredOutputSelection == audio::OutputDeviceSelection{});
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

    auto const result = DesktopSettingsYamlSchema{}.deserialize(tree.rootref(), DesktopSettings{});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotSupported);
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
      auto const result = deserialized(rejected);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotSupported);
    }

    for (auto const accepted : {std::uint32_t{2}, kDesktopSettingsVersion})
    {
      INFO("version " << accepted);
      auto const result = deserialized(accepted);
      REQUIRE(result);
      CHECK(result->version == kDesktopSettingsVersion);
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

      auto const result = DesktopSettingsYamlSchema{}.deserialize(tree.rootref(), seed);

      REQUIRE(result);
      CHECK(result->shellMode == ShellMode::Classic);
      CHECK(result->window == seed.window);
      CHECK(result->lastLibraryPath == "C:/Seeded");
      CHECK(result->preferredOutputSelection == seed.preferredOutputSelection);
      CHECK(result->navigationPaneWidth == 250.0);
      CHECK(result->inspectorPaneWidth == 330.0);
    }

    SECTION("partial window placement")
    {
      auto const* source = R"(
version: 3
window: {maximized: true}
)";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);

      auto const result = DesktopSettingsYamlSchema{}.deserialize(tree.rootref(), seed);

      REQUIRE(result);
      CHECK(result->window == WindowPlacement{.x = 5, .y = 6, .width = 1300, .height = 810, .maximized = true});
      CHECK(result->shellMode == seed.shellMode);
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
      auto result = DesktopSettingsYamlSchema{}.deserialize(tree.rootref(), DesktopSettings{});

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
      CHECK(result.error().message.contains("shell mode"));
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
      auto result = DesktopSettingsYamlSchema{}.deserialize(tree.rootref(), DesktopSettings{});

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
      CHECK(result.error().message.contains("future"));
    }

    SECTION("noncurrent version cannot be serialized")
    {
      auto state = DesktopSettings{};
      state.version = kDesktopSettingsVersion - 1;
      auto tree = ryml::Tree{yaml::callbacks()};

      auto result = DesktopSettingsYamlSchema{}.serialize(tree.rootref(), state);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotSupported);
    }

    SECTION("oversized navigation pane")
    {
      auto state = DesktopSettings{};
      state.navigationPaneWidth = kMaximumNavigationPaneWidth + 1.0;
      auto tree = ryml::Tree{yaml::callbacks()};

      auto result = DesktopSettingsYamlSchema{}.serialize(tree.rootref(), state);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
      CHECK(result.error().message.contains("pane widths"));
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
      auto result = DesktopSettingsYamlSchema{}.deserialize(tree.rootref(), DesktopSettings{});

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
      CHECK(result.error().message.contains("pane widths"));
    }
  }
} // namespace ao::winui::test
