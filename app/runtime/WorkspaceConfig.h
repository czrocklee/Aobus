// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <memory>

namespace ao::rt
{
  class CoreRuntime;
  class ViewService;
  class PlaybackService;
  class WorkspaceService;
  class ConfigStore;

  /**
   * @brief Manages persistence of library-specific workspace state.
   * 
   * This class exclusively owns the `library/workspace.yaml` file.
   */
  class WorkspaceConfig final
  {
  public:
    WorkspaceConfig(WorkspaceService& workspace,
                    ViewService& views,
                    PlaybackService& playback,
                    CoreRuntime& runtime,
                    ConfigStore& store);
    ~WorkspaceConfig();

    WorkspaceConfig(WorkspaceConfig const&) = delete;
    WorkspaceConfig& operator=(WorkspaceConfig const&) = delete;
    WorkspaceConfig(WorkspaceConfig&&) = delete;
    WorkspaceConfig& operator=(WorkspaceConfig&&) = delete;

    /**
     * @brief Restores the workspace session from the config store.
     */
    void restore();

    /**
     * @brief Saves the current workspace session to the config store and flushes.
     */
    void save();

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace ao::rt
