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
  std::filesystem::path layoutStatePath(std::filesystem::path const& musicRoot);
  Result<> validateConfigStorePaths(std::filesystem::path const& musicRoot,
                                    std::filesystem::path const& workspaceConfigPath,
                                    std::optional<std::filesystem::path> const& optAppConfigPath);

  class LayoutStateStore final
  {
  public:
    explicit LayoutStateStore(std::filesystem::path const& musicRoot);
    ~LayoutStateStore();

    LayoutStateStore(LayoutStateStore const&) = delete;
    LayoutStateStore& operator=(LayoutStateStore const&) = delete;
    LayoutStateStore(LayoutStateStore&&) noexcept;
    LayoutStateStore& operator=(LayoutStateStore&&) noexcept;

    void load(uimodel::TrackColumnLayouts::Snapshot& columnLayouts,
              uimodel::ListPresentations::Snapshot& listPresentations,
              bool& navigationEnabled) const;
    Result<> save(uimodel::TrackColumnLayouts::Snapshot const& columnLayouts,
                  uimodel::ListPresentations::Snapshot const& listPresentations,
                  bool navigationEnabled);

  private:
    std::unique_ptr<rt::ConfigStore> _storePtr;
  };
} // namespace ao::tui
