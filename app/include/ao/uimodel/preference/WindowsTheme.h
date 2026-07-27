// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <ryml.hpp>

#include <cstdint>
#include <string>

namespace ao::uimodel
{
  enum class WindowsClassicChrome : std::uint8_t
  {
    System,
    Retro,
  };

  struct WindowsSharedThemeTokens final
  {
    std::string fontFamily{"Segoe UI Variable Text"};
    std::string accent{"#06B6D4"};
    std::string windowBackground{"#111827"};
    std::string surface{"#1F2937"};
    std::string textPrimary{"#F9FAFB"};
    std::string textSecondary{"#9CA3AF"};
    std::string divider{"#374151"};
    std::string selection{"#334155"};

    friend bool operator==(WindowsSharedThemeTokens const&, WindowsSharedThemeTokens const&) = default;
  };

  struct WindowsModernThemeTokens final
  {
    std::string navigationBackground{"#0F172A"};
    std::string inspectorBackground{"#172033"};
    std::string nowPlayingBackground{"#0B1220"};

    friend bool operator==(WindowsModernThemeTokens const&, WindowsModernThemeTokens const&) = default;
  };

  struct WindowsClassicThemeTokens final
  {
    WindowsClassicChrome chrome = WindowsClassicChrome::System;
    std::string toolbarBackground{"#F3F4F6"};
    std::string treeBackground{"#FFFFFF"};
    std::string statusBackground{"#E5E7EB"};

    friend bool operator==(WindowsClassicThemeTokens const&, WindowsClassicThemeTokens const&) = default;
  };

  struct WindowsTheme final
  {
    WindowsSharedThemeTokens shared{};
    WindowsModernThemeTokens modern{};
    WindowsClassicThemeTokens classic{};

    friend bool operator==(WindowsTheme const&, WindowsTheme const&) = default;
  };

  struct WindowsThemeYamlSchema final
  {
    Result<> serialize(ryml::NodeRef node, WindowsTheme const& state) const;
    Result<WindowsTheme> deserialize(ryml::ConstNodeRef node, WindowsTheme const& seed) const;
  };

  class WindowsThemeSessionModel final
  {
  public:
    WindowsTheme const& theme() const noexcept { return _theme; }
    Result<> reload(ryml::ConstNodeRef node);

  private:
    WindowsTheme _theme{};
  };
} // namespace ao::uimodel
