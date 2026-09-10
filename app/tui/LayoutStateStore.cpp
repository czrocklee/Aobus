// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "LayoutStateStore.h"

#include "PanelWidths.h"
#include <ao/Error.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceYamlSchema.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutYamlSchema.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/yaml/Serialization.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifdef __APPLE__
#include <strings.h> // NOLINT(misc-include-cleaner) -- public Darwin header for strcasecmp.
#include <unistd.h>

#include <sys/unistd.h>
#endif

namespace ao::tui
{
  namespace
  {
    struct PanelWidthsSchema final
    {
      Result<> serialize(ryml::NodeRef node, PanelWidths const widths) const
      {
        if (widths.navigation < 0 || widths.detail < 0)
        {
          return makeError(Error::Code::InvalidInput, "Invalid TUI panel widths");
        }

        auto writer = yaml::MapWriter{node};
        writer.scalar("version", 1).scalar("navigation", widths.navigation).scalar("detail", widths.detail);
        return std::move(writer).finish();
      }

      Result<PanelWidths> deserialize(ryml::ConstNodeRef node, PanelWidths const /*seed*/) const
      {
        constexpr auto kKeys = std::to_array<std::string_view>({"version", "navigation", "detail"});
        std::int32_t version = 0;
        auto widths = PanelWidths{};
        auto reader = yaml::MapReader{node, kKeys, "TUI panel widths"};
        reader.requiredScalar("version", version)
          .requiredScalar("navigation", widths.navigation)
          .requiredScalar("detail", widths.detail);
        auto res = std::move(reader).finish(widths);

        if (!res)
        {
          return res;
        }

        if (version != 1)
        {
          return makeError(Error::Code::NotSupported, "Unsupported TUI panel widths version");
        }

        if (widths.navigation < 0 || widths.detail < 0)
        {
          return makeError(Error::Code::InvalidInput, "Invalid TUI panel widths");
        }

        return res;
      }
    };

    struct NavigationSchema final
    {
      Result<> serialize(ryml::NodeRef node, bool const enabled) const
      {
        auto writer = yaml::MapWriter{node};
        writer.scalar("version", 1).scalar("enabled", enabled);
        return std::move(writer).finish();
      }

      Result<bool> deserialize(ryml::ConstNodeRef node, bool const /*seed*/) const
      {
        constexpr auto kKeys = std::to_array<std::string_view>({"version", "enabled"});
        std::int32_t version = 0;
        bool enabled = true;
        auto reader = yaml::MapReader{node, kKeys, "TUI navigation"};
        reader.requiredScalar("version", version).requiredScalar("enabled", enabled);
        auto res = std::move(reader).finish(enabled);

        if (!res)
        {
          return res;
        }

        if (version != 1)
        {
          return makeError(Error::Code::NotSupported, "Unsupported TUI navigation version");
        }

        return res;
      }
    };

    std::filesystem::path normalizedPhysicalPath(std::filesystem::path const& path)
    {
      auto ec = std::error_code{};
      auto normalized = std::filesystem::weakly_canonical(path, ec);

      if (!ec)
      {
        return normalized;
      }

      ec.clear();
      normalized = std::filesystem::absolute(path, ec);
      return (ec ? path : normalized).lexically_normal();
    }

#ifdef __APPLE__
    bool canIgnorePathCase(std::filesystem::path path)
    {
      auto ec = std::error_code{};

      while (!path.empty() && !std::filesystem::exists(path, ec))
      {
        ec.clear();
        auto const parent = path.parent_path();

        if (parent == path)
        {
          break;
        }

        path = parent;
      }

      return path.empty() || ::pathconf(path.c_str(), _PC_CASE_SENSITIVE) != 1;
    }
#endif

    bool isSameFilePath(std::filesystem::path const& left, std::filesystem::path const& right)
    {
      if (auto ec = std::error_code{}; std::filesystem::equivalent(left, right, ec))
      {
        return true;
      }

      auto const normalizedLeft = normalizedPhysicalPath(left);
      auto const normalizedRight = normalizedPhysicalPath(right);

#ifdef _WIN32
      return normalizedLeft == normalizedRight ||
             ::CompareStringOrdinal(normalizedLeft.c_str(), -1, normalizedRight.c_str(), -1, TRUE) == CSTR_EQUAL;
#elifdef __APPLE__
      return normalizedLeft == normalizedRight ||
             (canIgnorePathCase(normalizedLeft) && canIgnorePathCase(normalizedRight) &&
              // Darwin exposes strcasecmp through <strings.h>, backed by an SDK-private declaration header.
              // NOLINTNEXTLINE(misc-include-cleaner)
              ::strcasecmp(normalizedLeft.c_str(), normalizedRight.c_str()) == 0);
#else
      return normalizedLeft == normalizedRight;
#endif
    }
  } // namespace

  std::filesystem::path layoutStatePath(std::filesystem::path const& musicRoot)
  {
    return rt::LibraryPaths{musicRoot}.managedDataPath() / "tui_layout.yaml";
  }

  Result<> validateConfigStorePaths(std::filesystem::path const& musicRoot,
                                    std::filesystem::path const& workspaceConfigPath,
                                    std::optional<std::filesystem::path> const& optAppConfigPath)
  {
    auto const layoutPath = layoutStatePath(musicRoot);

    if (isSameFilePath(workspaceConfigPath, layoutPath))
    {
      return makeError(
        Error::Code::InvalidInput, "The TUI workspace configuration path aliases the TUI layout-state file");
    }

    if (optAppConfigPath && isSameFilePath(workspaceConfigPath, *optAppConfigPath))
    {
      return makeError(
        Error::Code::InvalidInput, "The TUI workspace configuration path aliases the TUI application-preference file");
    }

    if (optAppConfigPath && isSameFilePath(layoutPath, *optAppConfigPath))
    {
      return makeError(
        Error::Code::InvalidInput, "The TUI layout-state path aliases the TUI application-preference file");
    }

    return {};
  }

  LayoutStateStore::LayoutStateStore(std::filesystem::path const& musicRoot)
  {
    auto const configPath = layoutStatePath(musicRoot);
    auto ec = std::error_code{};
    std::filesystem::create_directories(configPath.parent_path(), ec);

    if (ec)
    {
      APP_LOG_WARN("TUI: failed to prepare the layout-state directory: {}", ec.message());
    }

    _storePtr = std::make_unique<rt::ConfigStore>(configPath);
  }

  LayoutStateStore::~LayoutStateStore() = default;

  LayoutStateStore::LayoutStateStore(LayoutStateStore&&) noexcept = default;
  LayoutStateStore& LayoutStateStore::operator=(LayoutStateStore&&) noexcept = default;

  void LayoutStateStore::load(uimodel::TrackColumnLayouts::Snapshot& columnLayouts,
                              uimodel::ListPresentations::Snapshot& listPresentations,
                              bool& navigationEnabled,
                              PanelWidths& widths) const
  {
    widths = {};

    if (auto const res = _storePtr->load("panels", widths, PanelWidthsSchema{});
        !res && res.error().code != Error::Code::NotFound)
    {
      APP_LOG_WARN("TUI: failed to load panel widths: {}", res.error().message);
    }

    navigationEnabled = true;

    if (auto const res = _storePtr->load("navigation", navigationEnabled, NavigationSchema{});
        !res && res.error().code != Error::Code::NotFound)
    {
      APP_LOG_WARN("TUI: failed to load List navigation preference: {}", res.error().message);
    }

    auto const columnsRes =
      _storePtr->load(uimodel::kTrackColumnLayoutsConfigGroup, columnLayouts, uimodel::TrackColumnLayoutYamlSchema{});

    if (!columnsRes && columnsRes.error().code != Error::Code::NotFound)
    {
      APP_LOG_WARN("TUI: failed to load terminal column layouts: {}", columnsRes.error().message);
    }

    auto const presentationsRes = _storePtr->load(
      uimodel::kListPresentationsConfigGroup, listPresentations, uimodel::ListPresentationPreferenceYamlSchema{});

    if (!presentationsRes && presentationsRes.error().code != Error::Code::NotFound)
    {
      APP_LOG_WARN("TUI: failed to load list presentation preferences: {}", presentationsRes.error().message);
    }
  }

  Result<> LayoutStateStore::save(uimodel::TrackColumnLayouts::Snapshot const& columnLayouts,
                                  uimodel::ListPresentations::Snapshot const& listPresentations,
                                  bool const navigationEnabled,
                                  PanelWidths const widths)
  {
    return _storePtr->saveTogether(
      rt::configWrite("panels", widths, PanelWidthsSchema{}),
      rt::configWrite("navigation", navigationEnabled, NavigationSchema{}),
      rt::configWrite(uimodel::kTrackColumnLayoutsConfigGroup, columnLayouts, uimodel::TrackColumnLayoutYamlSchema{}),
      rt::configWrite(
        uimodel::kListPresentationsConfigGroup, listPresentations, uimodel::ListPresentationPreferenceYamlSchema{}));
  }
} // namespace ao::tui
