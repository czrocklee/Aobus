// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/uimodel/preference/WindowsTheme.h>
#include <ao/yaml/Serialization.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace ao::uimodel
{
  namespace
  {
    constexpr std::size_t kRgbColorLength = 7;
    constexpr std::size_t kArgbColorLength = 9;

    bool isColor(std::string_view const value) noexcept
    {
      if ((value.size() != kRgbColorLength && value.size() != kArgbColorLength) || value.front() != '#')
      {
        return false;
      }

      return std::ranges::all_of(value.substr(1),
                                 [](char const character)
                                 { return std::isxdigit(static_cast<unsigned char>(character)) != 0; });
    }

    Result<> validateColor(std::string_view const name, std::string_view const value)
    {
      if (!isColor(value))
      {
        return makeError(
          Error::Code::FormatRejected, std::format("Windows theme token '{}' is not #RRGGBB or #AARRGGBB", name));
      }

      return {};
    }

    Result<> validateTheme(WindowsTheme const& theme)
    {
      if (theme.shared.fontFamily.empty())
      {
        return makeError(Error::Code::FormatRejected, "Windows theme fontFamily cannot be empty");
      }

      auto result = Result<>{};
      auto const validate = [&result](std::string_view const name, std::string_view const value)
      {
        if (result)
        {
          result = validateColor(name, value);
        }
      };
      validate("shared.accent", theme.shared.accent);
      validate("shared.windowBackground", theme.shared.windowBackground);
      validate("shared.surface", theme.shared.surface);
      validate("shared.textPrimary", theme.shared.textPrimary);
      validate("shared.textSecondary", theme.shared.textSecondary);
      validate("shared.divider", theme.shared.divider);
      validate("shared.selection", theme.shared.selection);
      validate("modern.navigationBackground", theme.modern.navigationBackground);
      validate("modern.inspectorBackground", theme.modern.inspectorBackground);
      validate("modern.nowPlayingBackground", theme.modern.nowPlayingBackground);
      validate("classic.toolbarBackground", theme.classic.toolbarBackground);
      validate("classic.treeBackground", theme.classic.treeBackground);
      validate("classic.statusBackground", theme.classic.statusBackground);
      return result;
    }

    std::string_view chromeId(WindowsClassicChrome const chrome) noexcept
    {
      return chrome == WindowsClassicChrome::Retro ? "retro" : "system";
    }

    Result<WindowsClassicChrome> chromeFromId(std::string_view const id)
    {
      if (id == "system")
      {
        return WindowsClassicChrome::System;
      }

      if (id == "retro")
      {
        return WindowsClassicChrome::Retro;
      }

      return makeError(Error::Code::FormatRejected, std::format("Unknown classic.chrome value '{}'", id));
    }

    Result<> writeShared(ryml::NodeRef node, WindowsSharedThemeTokens const& value)
    {
      auto writer = yaml::MapWriter{node};
      writer.scalar("fontFamily", value.fontFamily)
        .scalar("accent", value.accent)
        .scalar("windowBackground", value.windowBackground)
        .scalar("surface", value.surface)
        .scalar("textPrimary", value.textPrimary)
        .scalar("textSecondary", value.textSecondary)
        .scalar("divider", value.divider)
        .scalar("selection", value.selection);
      return {};
    }

    Result<WindowsSharedThemeTokens> readShared(ryml::ConstNodeRef node, std::string_view context)
    {
      constexpr auto kKeys = std::to_array<std::string_view>({"fontFamily",
                                                              "accent",
                                                              "windowBackground",
                                                              "surface",
                                                              "textPrimary",
                                                              "textSecondary",
                                                              "divider",
                                                              "selection"});
      auto value = WindowsSharedThemeTokens{};
      auto reader = yaml::MapReader{node, kKeys, context};
      reader.requiredScalar("fontFamily", value.fontFamily)
        .requiredScalar("accent", value.accent)
        .requiredScalar("windowBackground", value.windowBackground)
        .requiredScalar("surface", value.surface)
        .requiredScalar("textPrimary", value.textPrimary)
        .requiredScalar("textSecondary", value.textSecondary)
        .requiredScalar("divider", value.divider)
        .requiredScalar("selection", value.selection);
      return std::move(reader).finish(std::move(value));
    }

    Result<> writeModern(ryml::NodeRef node, WindowsModernThemeTokens const& value)
    {
      auto writer = yaml::MapWriter{node};
      writer.scalar("navigationBackground", value.navigationBackground)
        .scalar("inspectorBackground", value.inspectorBackground)
        .scalar("nowPlayingBackground", value.nowPlayingBackground);
      return {};
    }

    Result<WindowsModernThemeTokens> readModern(ryml::ConstNodeRef node, std::string_view context)
    {
      constexpr auto kKeys =
        std::to_array<std::string_view>({"navigationBackground", "inspectorBackground", "nowPlayingBackground"});
      auto value = WindowsModernThemeTokens{};
      auto reader = yaml::MapReader{node, kKeys, context};
      reader.requiredScalar("navigationBackground", value.navigationBackground)
        .requiredScalar("inspectorBackground", value.inspectorBackground)
        .requiredScalar("nowPlayingBackground", value.nowPlayingBackground);
      return std::move(reader).finish(std::move(value));
    }

    Result<> writeClassic(ryml::NodeRef node, WindowsClassicThemeTokens const& value)
    {
      auto writer = yaml::MapWriter{node};
      writer.scalar("chrome", chromeId(value.chrome))
        .scalar("toolbarBackground", value.toolbarBackground)
        .scalar("treeBackground", value.treeBackground)
        .scalar("statusBackground", value.statusBackground);
      return {};
    }

    Result<WindowsClassicThemeTokens> readClassic(ryml::ConstNodeRef node, std::string_view context)
    {
      constexpr auto kKeys =
        std::to_array<std::string_view>({"chrome", "toolbarBackground", "treeBackground", "statusBackground"});
      auto value = WindowsClassicThemeTokens{};
      auto chrome = std::string{};
      auto reader = yaml::MapReader{node, kKeys, context};
      reader.requiredScalar("chrome", chrome)
        .requiredScalar("toolbarBackground", value.toolbarBackground)
        .requiredScalar("treeBackground", value.treeBackground)
        .requiredScalar("statusBackground", value.statusBackground);
      auto result = std::move(reader).finish(std::move(value));

      if (!result)
      {
        return result;
      }

      auto parsedChrome = chromeFromId(chrome);

      if (!parsedChrome)
      {
        return std::unexpected{parsedChrome.error()};
      }

      result->chrome = *parsedChrome;
      return result;
    }
  } // namespace

  Result<> WindowsThemeYamlSchema::serialize(ryml::NodeRef node, WindowsTheme const& state) const
  {
    if (auto const valid = validateTheme(state); !valid)
    {
      return valid;
    }

    auto writer = yaml::MapWriter{node};
    writer.value("shared", state.shared, writeShared)
      .value("modern", state.modern, writeModern)
      .value("classic", state.classic, writeClassic);
    return std::move(writer).finish();
  }

  Result<WindowsTheme> WindowsThemeYamlSchema::deserialize(ryml::ConstNodeRef node, WindowsTheme const& /*seed*/) const
  {
    constexpr auto kKeys = std::to_array<std::string_view>({"shared", "modern", "classic"});
    auto state = WindowsTheme{};
    auto reader = yaml::MapReader{node, kKeys, "Windows theme"};
    reader.requiredValue("shared", state.shared, readShared)
      .requiredValue("modern", state.modern, readModern)
      .requiredValue("classic", state.classic, readClassic);
    auto result = std::move(reader).finish(std::move(state));

    if (!result)
    {
      return result;
    }

    if (auto const valid = validateTheme(*result); !valid)
    {
      return std::unexpected{valid.error()};
    }

    return result;
  }

  Result<> WindowsThemeSessionModel::reload(ryml::ConstNodeRef node)
  {
    auto candidate = WindowsThemeYamlSchema{}.deserialize(node, _theme);

    if (!candidate)
    {
      return std::unexpected{candidate.error()};
    }

    _theme = std::move(*candidate);
    return {};
  }
} // namespace ao::uimodel
