// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>

#include <filesystem>
#include <memory>
#include <optional>

namespace ao::rt
{
  class ConfigStore;
}

namespace ao::tui
{
  std::filesystem::path tuiLayoutStatePath(std::filesystem::path const& musicRoot);
  Result<> validateTuiConfigStorePaths(std::filesystem::path const& musicRoot,
                                       std::filesystem::path const& workspaceConfigPath,
                                       std::optional<std::filesystem::path> const& optAppConfigPath);

  class TuiLayoutStateStore final
  {
  public:
    explicit TuiLayoutStateStore(std::filesystem::path const& musicRoot);
    ~TuiLayoutStateStore();

    TuiLayoutStateStore(TuiLayoutStateStore const&) = delete;
    TuiLayoutStateStore& operator=(TuiLayoutStateStore const&) = delete;
    TuiLayoutStateStore(TuiLayoutStateStore&&) noexcept;
    TuiLayoutStateStore& operator=(TuiLayoutStateStore&&) noexcept;

    void load(uimodel::TrackColumnLayouts::Snapshot& columnLayouts,
              uimodel::ListPresentations::Snapshot& listPresentations) const;
    Result<> save(uimodel::TrackColumnLayouts::Snapshot const& columnLayouts,
                  uimodel::ListPresentations::Snapshot const& listPresentations);

  private:
    std::unique_ptr<rt::ConfigStore> _storePtr;
  };
} // namespace ao::tui
