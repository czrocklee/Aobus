// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/Theme.h>

#include <ao/Error.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::winui::test
{
  namespace
  {
    ryml::Tree themeTree(char const* source)
    {
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      return tree;
    }

    constexpr auto kValidTheme = R"(
shared:
  fontFamily: Segoe UI Variable Text
  accent: '#06B6D4'
  windowBackground: '#111827'
  surface: '#1F2937'
  textPrimary: '#F9FAFB'
  textSecondary: '#9CA3AF'
  divider: '#374151'
  selection: '#334155'
modern:
  navigationBackground: '#0F172A'
  inspectorBackground: '#172033'
  nowPlayingBackground: '#0B1220'
classic:
  chrome: system
  toolbarBackground: '#F3F4F6'
  treeBackground: '#FFFFFF'
  statusBackground: '#E5E7EB'
)";
  } // namespace

  TEST_CASE("Theme - valid semantic tokens replace the complete active theme", "[winui][unit][theme]")
  {
    auto model = ThemeSessionModel{};
    auto tree = themeTree(kValidTheme);

    REQUIRE(model.reload(tree.rootref()));
    CHECK(model.theme().shared.accent == "#06B6D4");
    CHECK(model.theme().modern.nowPlayingBackground == "#0B1220");
    CHECK(model.theme().classic.chrome == ClassicChrome::System);
  }

  TEST_CASE("Theme - invalid reload preserves the last valid theme", "[winui][unit][theme]")
  {
    auto model = ThemeSessionModel{};
    auto valid = themeTree(kValidTheme);
    REQUIRE(model.reload(valid.rootref()));
    auto const previous = model.theme();

    SECTION("unknown token")
    {
      auto invalid = themeTree(R"(
shared:
  fontFamily: Segoe UI
  accent: '#06B6D4'
  windowBackground: '#111827'
  surface: '#1F2937'
  textPrimary: '#F9FAFB'
  textSecondary: '#9CA3AF'
  divider: '#374151'
  selection: '#334155'
  future: true
modern: {navigationBackground: '#0F172A', inspectorBackground: '#172033', nowPlayingBackground: '#0B1220'}
classic: {chrome: system, toolbarBackground: '#F3F4F6', treeBackground: '#FFFFFF', statusBackground: '#E5E7EB'}
)");
      auto result = model.reload(invalid.rootref());
      REQUIRE_FALSE(result);
      CHECK(result.error().message.contains("future"));
    }

    SECTION("invalid color")
    {
      auto invalid = themeTree(R"(
shared:
  fontFamily: Segoe UI
  accent: cyan
  windowBackground: '#111827'
  surface: '#1F2937'
  textPrimary: '#F9FAFB'
  textSecondary: '#9CA3AF'
  divider: '#374151'
  selection: '#334155'
modern: {navigationBackground: '#0F172A', inspectorBackground: '#172033', nowPlayingBackground: '#0B1220'}
classic: {chrome: system, toolbarBackground: '#F3F4F6', treeBackground: '#FFFFFF', statusBackground: '#E5E7EB'}
)");
      auto result = model.reload(invalid.rootref());
      REQUIRE_FALSE(result);
      CHECK(result.error().message.contains("shared.accent"));
    }

    SECTION("invalid retro selector")
    {
      auto invalid = themeTree(R"(
shared: {fontFamily: Segoe UI, accent: '#06B6D4', windowBackground: '#111827', surface: '#1F2937', textPrimary: '#F9FAFB', textSecondary: '#9CA3AF', divider: '#374151', selection: '#334155'}
modern: {navigationBackground: '#0F172A', inspectorBackground: '#172033', nowPlayingBackground: '#0B1220'}
classic: {chrome: arbitrary, toolbarBackground: '#F3F4F6', treeBackground: '#FFFFFF', statusBackground: '#E5E7EB'}
)");
      auto result = model.reload(invalid.rootref());
      REQUIRE_FALSE(result);
      CHECK(result.error().message.contains("classic.chrome"));
    }

    CHECK(model.theme() == previous);
  }

  TEST_CASE("Theme - retro chrome is a closed built-in choice", "[winui][unit][theme]")
  {
    auto tree = themeTree(R"(
shared: {fontFamily: Segoe UI, accent: '#06B6D4', windowBackground: '#111827', surface: '#1F2937', textPrimary: '#F9FAFB', textSecondary: '#9CA3AF', divider: '#374151', selection: '#334155'}
modern: {navigationBackground: '#0F172A', inspectorBackground: '#172033', nowPlayingBackground: '#0B1220'}
classic: {chrome: retro, toolbarBackground: '#F3F4F6', treeBackground: '#FFFFFF', statusBackground: '#E5E7EB'}
)");
    auto result = ThemeYamlSchema{}.deserialize(tree.rootref(), Theme{});

    REQUIRE(result);
    CHECK(result->classic.chrome == ClassicChrome::Retro);
  }
} // namespace ao::winui::test
