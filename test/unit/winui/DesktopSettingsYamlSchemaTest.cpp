// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/DesktopSettingsYamlSchema.h>

#include <ao/Error.h>
#include <ao/winui/layout/ShellStatePolicy.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace ao::winui::test
{
  TEST_CASE("DesktopSettingsYamlSchema - round-trip owns independent desktop state", "[winui][unit][layout]")
  {
    auto state = DesktopSettings{};
    state.window = {.x = 120, .y = 140, .width = 1440, .height = 900, .maximized = true};
    state.shellMode = ShellMode::Classic;
    state.lastLibraryPath = "C:/Music";
    state.navigationPaneWidth = 260.0;
    state.inspectorPaneWidth = 360.0;

    auto tree = ryml::Tree{yaml::callbacks()};
    REQUIRE(DesktopSettingsYamlSchema{}.serialize(tree.rootref(), state));
    auto decoded = DesktopSettingsYamlSchema{}.deserialize(tree.rootref(), DesktopSettings{});

    REQUIRE(decoded);
    CHECK(*decoded == state);
  }

  TEST_CASE("DesktopSettingsYamlSchema - accepts pane width boundaries", "[winui][unit][layout]")
  {
    auto state = DesktopSettings{};
    state.navigationPaneWidth = kMaximumNavigationPaneWidth;
    state.inspectorPaneWidth = kMinimumInspectorPaneWidth;

    auto tree = ryml::Tree{yaml::callbacks()};
    REQUIRE(DesktopSettingsYamlSchema{}.serialize(tree.rootref(), state));
    auto decoded = DesktopSettingsYamlSchema{}.deserialize(tree.rootref(), DesktopSettings{});

    REQUIRE(decoded);
    CHECK(decoded->navigationPaneWidth == kMaximumNavigationPaneWidth);
    CHECK(decoded->inspectorPaneWidth == kMinimumInspectorPaneWidth);
  }

  TEST_CASE("DesktopSettingsYamlSchema - rejects noncanonical persisted state", "[winui][unit][layout]")
  {
    SECTION("unknown shell mode")
    {
      auto const* source = R"(
version: 2
window: {x: 0, y: 0, width: 1280, height: 800, maximized: false}
shellMode: future
lastLibraryPath: ''
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
version: 2
window: {x: 0, y: 0, width: 1280, height: 800, maximized: false}
shellMode: modern
lastLibraryPath: ''
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

    SECTION("legacy version")
    {
      auto state = DesktopSettings{};
      state.version = 1;
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
version: 2
window: {x: 0, y: 0, width: 1280, height: 800, maximized: false}
shellMode: modern
lastLibraryPath: ''
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
