// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TuiLayoutStateStore.h"

#include <ao/Error.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceYamlSchema.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutYamlSchema.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <system_error>

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
#include <strings.h>
#include <unistd.h>
#endif

namespace ao::tui
{
  namespace
  {
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
    bool pathMayIgnoreCase(std::filesystem::path path)
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

    bool pathsReferToSameFile(std::filesystem::path const& left, std::filesystem::path const& right)
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
             (pathMayIgnoreCase(normalizedLeft) && pathMayIgnoreCase(normalizedRight) &&
              ::strcasecmp(normalizedLeft.c_str(), normalizedRight.c_str()) == 0);
#else
      return normalizedLeft == normalizedRight;
#endif
    }
  } // namespace

  std::filesystem::path tuiLayoutStatePath(std::filesystem::path const& musicRoot)
  {
    return rt::LibraryPaths{musicRoot}.managedDataPath() / "tui_layout.yaml";
  }

  Result<> validateTuiConfigStorePaths(std::filesystem::path const& musicRoot,
                                       std::filesystem::path const& workspaceConfigPath,
                                       std::optional<std::filesystem::path> const& optAppConfigPath)
  {
    auto const layoutPath = tuiLayoutStatePath(musicRoot);

    if (pathsReferToSameFile(workspaceConfigPath, layoutPath))
    {
      return makeError(
        Error::Code::InvalidInput, "The TUI workspace configuration path aliases the TUI layout-state file");
    }

    if (optAppConfigPath && pathsReferToSameFile(workspaceConfigPath, *optAppConfigPath))
    {
      return makeError(
        Error::Code::InvalidInput, "The TUI workspace configuration path aliases the TUI application-preference file");
    }

    if (optAppConfigPath && pathsReferToSameFile(layoutPath, *optAppConfigPath))
    {
      return makeError(
        Error::Code::InvalidInput, "The TUI layout-state path aliases the TUI application-preference file");
    }

    return {};
  }

  TuiLayoutStateStore::TuiLayoutStateStore(std::filesystem::path const& musicRoot)
  {
    auto const configPath = tuiLayoutStatePath(musicRoot);
    auto ec = std::error_code{};
    std::filesystem::create_directories(configPath.parent_path(), ec);

    if (ec)
    {
      APP_LOG_WARN("TUI: failed to prepare the layout-state directory: {}", ec.message());
    }

    _storePtr = std::make_unique<rt::ConfigStore>(configPath);
  }

  TuiLayoutStateStore::~TuiLayoutStateStore() = default;

  TuiLayoutStateStore::TuiLayoutStateStore(TuiLayoutStateStore&&) noexcept = default;
  TuiLayoutStateStore& TuiLayoutStateStore::operator=(TuiLayoutStateStore&&) noexcept = default;

  void TuiLayoutStateStore::load(uimodel::TrackColumnLayouts::Snapshot& columnLayouts,
                                 uimodel::ListPresentations::Snapshot& listPresentations) const
  {
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

  Result<> TuiLayoutStateStore::save(uimodel::TrackColumnLayouts::Snapshot const& columnLayouts,
                                     uimodel::ListPresentations::Snapshot const& listPresentations)
  {
    return _storePtr->saveTogether(
      rt::configWrite(uimodel::kTrackColumnLayoutsConfigGroup, columnLayouts, uimodel::TrackColumnLayoutYamlSchema{}),
      rt::configWrite(
        uimodel::kListPresentationsConfigGroup, listPresentations, uimodel::ListPresentationPreferenceYamlSchema{}));
  }
} // namespace ao::tui
