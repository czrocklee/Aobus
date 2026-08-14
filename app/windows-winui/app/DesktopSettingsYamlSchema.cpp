// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/DesktopSettingsYamlSchema.h>

#include <ao/Error.h>
#include <ao/winui/layout/ShellStatePolicy.h>
#include <ao/yaml/Serialization.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace ao::winui
{
  namespace
  {
    std::string_view shellModeId(ShellMode const mode) noexcept
    {
      return mode == ShellMode::Classic ? "classic" : "modern";
    }

    Result<ShellMode> shellModeFromId(std::string_view const id)
    {
      if (id == "modern")
      {
        return ShellMode::Modern;
      }

      if (id == "classic")
      {
        return ShellMode::Classic;
      }

      return makeError(Error::Code::FormatRejected, std::format("Unknown Windows shell mode '{}'", id));
    }

    Result<> writeWindow(ryml::NodeRef node, WindowPlacement const& window)
    {
      auto writer = yaml::MapWriter{node};
      writer.scalar("x", window.x)
        .scalar("y", window.y)
        .scalar("width", window.width)
        .scalar("height", window.height)
        .scalar("maximized", window.maximized);
      return {};
    }

    Result<WindowPlacement> readWindow(ryml::ConstNodeRef node, std::string_view context)
    {
      constexpr auto kKeys = std::to_array<std::string_view>({"x", "y", "width", "height", "maximized"});
      auto window = WindowPlacement{};
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

      if (result->width < kMinimumWindowWidth || result->height < kMinimumWindowHeight)
      {
        return makeError(Error::Code::FormatRejected, "Windows window size must be at least 640x480");
      }

      return result;
    }

    Result<> requireCurrentVersion(std::uint32_t const version)
    {
      if (version != kDesktopSettingsVersion)
      {
        return makeError(
          Error::Code::NotSupported, std::format("Unsupported Windows desktop settings version {}", version));
      }

      return {};
    }

    Result<> validateSettingsValues(DesktopSettings const& state)
    {
      if (!std::isfinite(state.navigationPaneWidth) || state.navigationPaneWidth < kMinimumNavigationPaneWidth ||
          state.navigationPaneWidth > kMaximumNavigationPaneWidth || !std::isfinite(state.inspectorPaneWidth) ||
          state.inspectorPaneWidth < kMinimumInspectorPaneWidth ||
          state.inspectorPaneWidth > kMaximumInspectorPaneWidth)
      {
        return makeError(Error::Code::FormatRejected, "Windows desktop pane widths are invalid");
      }

      return {};
    }
  } // namespace

  Result<> DesktopSettingsYamlSchema::serialize(ryml::NodeRef node, DesktopSettings const& state) const
  {
    if (auto const versionRes = requireCurrentVersion(state.version); !versionRes)
    {
      return versionRes;
    }

    if (auto const validRes = validateSettingsValues(state); !validRes)
    {
      return validRes;
    }

    auto writer = yaml::MapWriter{node};
    writer.scalar("version", state.version)
      .value("window", state.window, writeWindow)
      .scalar("shellMode", shellModeId(state.shellMode))
      .scalar("lastLibraryPath", state.lastLibraryPath)
      .scalar("lastOutputBackendId", state.preferredOutputSelection.backendId)
      .scalar("lastOutputProfileId", state.preferredOutputSelection.profileId)
      .scalar("lastOutputDeviceId", state.preferredOutputSelection.deviceId)
      .scalar("navigationPaneWidth", state.navigationPaneWidth)
      .scalar("inspectorPaneWidth", state.inspectorPaneWidth);
    return std::move(writer).finish();
  }

  Result<DesktopSettings> DesktopSettingsYamlSchema::deserialize(ryml::ConstNodeRef node,
                                                                 DesktopSettings const& /*seed*/) const
  {
    constexpr auto kContext = std::string_view{"Windows desktop settings"};

    if (auto const result = yaml::requireMap(node, kContext); !result)
    {
      return std::unexpected{result.error()};
    }

    auto versionRes = yaml::requireScalar<std::uint32_t>(node, "version", kContext);

    if (!versionRes)
    {
      return std::unexpected{versionRes.error()};
    }

    if (auto const supportedRes = requireCurrentVersion(*versionRes); !supportedRes)
    {
      return std::unexpected{supportedRes.error()};
    }

    constexpr auto kKeys = std::to_array<std::string_view>({"version",
                                                            "window",
                                                            "shellMode",
                                                            "lastLibraryPath",
                                                            "lastOutputBackendId",
                                                            "lastOutputProfileId",
                                                            "lastOutputDeviceId",
                                                            "navigationPaneWidth",
                                                            "inspectorPaneWidth"});

    auto state = DesktopSettings{};
    state.version = *versionRes;
    auto modeId = std::string{};
    auto reader = yaml::MapReader{node, kKeys, kContext};
    reader.requiredValue("window", state.window, readWindow)
      .requiredScalar("shellMode", modeId)
      .requiredScalar("lastLibraryPath", state.lastLibraryPath)
      .requiredScalar("lastOutputBackendId", state.preferredOutputSelection.backendId)
      .requiredScalar("lastOutputProfileId", state.preferredOutputSelection.profileId)
      .requiredScalar("lastOutputDeviceId", state.preferredOutputSelection.deviceId)
      .requiredScalar("navigationPaneWidth", state.navigationPaneWidth)
      .requiredScalar("inspectorPaneWidth", state.inspectorPaneWidth);
    auto result = std::move(reader).finish(std::move(state));

    if (!result)
    {
      return result;
    }

    auto modeRes = shellModeFromId(modeId);

    if (!modeRes)
    {
      return std::unexpected{modeRes.error()};
    }

    result->shellMode = *modeRes;

    if (auto const validRes = validateSettingsValues(*result); !validRes)
    {
      return std::unexpected{validRes.error()};
    }

    return result;
  }
} // namespace ao::winui
