// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "CorePrimitives.h"
#include <functional>
#include <memory>
#include <string>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class ViewService;
  class PlaybackService;
  class WorkspaceService;
  class ConfigStore;

  /**
   * Manages saving and restoring the application session state (open views,
   * playback output, library path) to/from persistent storage.
   */
  class SessionPersistenceService final
  {
  public:
    SessionPersistenceService(WorkspaceService& workspace,
                              ViewService& views,
                              PlaybackService& playback,
                              library::MusicLibrary& library,
                              ConfigStore& configStore);
    ~SessionPersistenceService();

    SessionPersistenceService(SessionPersistenceService const&) = delete;
    SessionPersistenceService& operator=(SessionPersistenceService const&) = delete;
    SessionPersistenceService(SessionPersistenceService&&) = delete;
    SessionPersistenceService& operator=(SessionPersistenceService&&) = delete;

    /**
     * Restores the session from the config store.
     */
    void restore();

    /**
     * Saves the current session to the config store.
     */
    void save();

    /**
     * Signal emitted when the session has been restored, carrying the last
     * library path used.
     */
    Subscription onSessionRestored(std::move_only_function<void(std::string const&)> handler);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace ao::rt
