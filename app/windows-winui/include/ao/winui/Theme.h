// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <ryml.hpp>

#include <cstdint>
#include <string>

namespace ao::winui
{
  enum class ClassicChrome : std::uint8_t
  {
    System,
    Retro,
  };

  struct SharedThemeTokens final
  {
    std::string fontFamily{"Segoe UI Variable Text"};
    std::string accent{"#06B6D4"};
    std::string windowBackground{"#111827"};
    std::string surface{"#1F2937"};
    std::string textPrimary{"#F9FAFB"};
    std::string textSecondary{"#9CA3AF"};
    std::string divider{"#374151"};
    std::string selection{"#334155"};

    friend bool operator==(SharedThemeTokens const&, SharedThemeTokens const&) = default;
  };

  struct ModernThemeTokens final
  {
    std::string navigationBackground{"#0F172A"};
    std::string inspectorBackground{"#172033"};
    std::string nowPlayingBackground{"#0B1220"};

    friend bool operator==(ModernThemeTokens const&, ModernThemeTokens const&) = default;
  };

  struct ClassicThemeTokens final
  {
    ClassicChrome chrome = ClassicChrome::System;
    std::string toolbarBackground{"#F3F4F6"};
    std::string treeBackground{"#FFFFFF"};
    std::string statusBackground{"#E5E7EB"};

    friend bool operator==(ClassicThemeTokens const&, ClassicThemeTokens const&) = default;
  };

  struct Theme final
  {
    SharedThemeTokens shared{};
    ModernThemeTokens modern{};
    ClassicThemeTokens classic{};

    friend bool operator==(Theme const&, Theme const&) = default;
  };

  struct ThemeYamlSchema final
  {
    Result<> serialize(ryml::NodeRef node, Theme const& state) const;
    Result<Theme> deserialize(ryml::ConstNodeRef node, Theme const& seed) const;
  };

  class ThemeSessionModel final
  {
  public:
    Theme const& theme() const noexcept { return _theme; }
    Result<> reload(ryml::ConstNodeRef node);

  private:
    Theme _theme{};
  };
} // namespace ao::winui
