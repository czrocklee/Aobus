// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/uimodel/layout/shell/DesktopShellPolicy.h>
#include <ao/uimodel/layout/shell/WindowsDesktopSettingsYamlSchema.h>
#include <ao/yaml/Serialization.h>

#include <array>
#include <cmath>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace ao::uimodel
{
  namespace
  {
    std::string_view shellModeId(DesktopShellMode const mode) noexcept
    {
      return mode == DesktopShellMode::Classic ? "classic" : "modern";
    }

    Result<DesktopShellMode> shellModeFromId(std::string_view const id)
    {
      if (id == "modern")
      {
        return DesktopShellMode::Modern;
      }

      if (id == "classic")
      {
        return DesktopShellMode::Classic;
      }

      return makeError(Error::Code::FormatRejected, std::format("Unknown Windows shell mode '{}'", id));
    }

    Result<> writeWindow(ryml::NodeRef node, WindowsWindowPlacement const& window)
    {
      auto writer = yaml::MapWriter{node};
      writer.scalar("x", window.x)
        .scalar("y", window.y)
        .scalar("width", window.width)
        .scalar("height", window.height)
        .scalar("maximized", window.maximized);
      return {};
    }

    Result<WindowsWindowPlacement> readWindow(ryml::ConstNodeRef node, std::string_view context)
    {
      constexpr auto kKeys = std::to_array<std::string_view>({"x", "y", "width", "height", "maximized"});
      auto window = WindowsWindowPlacement{};
      auto reader = yaml::MapReader{node, kKeys, context};
      reader.requiredScalar("x", window.x)
        .requiredScalar("y", window.y)
        .requiredScalar("width", window.width)
        .requiredScalar("height", window.height)
        .requiredScalar("maximized", window.maximized);
      auto result = std::move(reader).finish(std::move(window));

      if (!result)
      {
        return result;
      }

      if (result->width < kMinimumWindowsWindowWidth || result->height < kMinimumWindowsWindowHeight)
      {
        return makeError(Error::Code::FormatRejected, "Windows window size must be at least 640x480");
      }

      return result;
    }

    Result<> validateSettings(WindowsDesktopSettings const& state)
    {
      if (state.version != kWindowsDesktopSettingsVersion)
      {
        return makeError(
          Error::Code::NotSupported, std::format("Unsupported Windows desktop settings version {}", state.version));
      }

      if (!std::isfinite(state.navigationPaneWidth) || state.navigationPaneWidth < kMinimumWindowsNavigationPaneWidth ||
          state.navigationPaneWidth > kMaximumWindowsNavigationPaneWidth || !std::isfinite(state.inspectorPaneWidth) ||
          state.inspectorPaneWidth < kMinimumWindowsInspectorPaneWidth ||
          state.inspectorPaneWidth > kMaximumWindowsInspectorPaneWidth)
      {
        return makeError(Error::Code::FormatRejected, "Windows desktop pane widths are invalid");
      }

      return {};
    }
  } // namespace

  Result<> WindowsDesktopSettingsYamlSchema::serialize(ryml::NodeRef node, WindowsDesktopSettings const& state) const
  {
    if (auto const valid = validateSettings(state); !valid)
    {
      return valid;
    }

    auto writer = yaml::MapWriter{node};
    writer.scalar("version", state.version)
      .value("window", state.window, writeWindow)
      .scalar("shellMode", shellModeId(state.shellMode))
      .scalar("lastLibraryPath", state.lastLibraryPath)
      .scalar("navigationPaneWidth", state.navigationPaneWidth)
      .scalar("inspectorPaneWidth", state.inspectorPaneWidth);
    return std::move(writer).finish();
  }

  Result<WindowsDesktopSettings> WindowsDesktopSettingsYamlSchema::deserialize(
    ryml::ConstNodeRef node,
    WindowsDesktopSettings const& /*seed*/) const
  {
    constexpr auto kKeys = std::to_array<std::string_view>(
      {"version", "window", "shellMode", "lastLibraryPath", "navigationPaneWidth", "inspectorPaneWidth"});
    constexpr auto kContext = std::string_view{"Windows desktop settings"};

    auto state = WindowsDesktopSettings{};
    auto modeId = std::string{};
    auto reader = yaml::MapReader{node, kKeys, kContext};
    reader.requiredScalar("version", state.version)
      .requiredValue("window", state.window, readWindow)
      .requiredScalar("shellMode", modeId)
      .requiredScalar("lastLibraryPath", state.lastLibraryPath)
      .requiredScalar("navigationPaneWidth", state.navigationPaneWidth)
      .requiredScalar("inspectorPaneWidth", state.inspectorPaneWidth);
    auto result = std::move(reader).finish(std::move(state));

    if (!result)
    {
      return result;
    }

    auto mode = shellModeFromId(modeId);

    if (!mode)
    {
      return std::unexpected{mode.error()};
    }

    result->shellMode = *mode;

    if (auto const valid = validateSettings(*result); !valid)
    {
      return std::unexpected{valid.error()};
    }

    return result;
  }
} // namespace ao::uimodel
