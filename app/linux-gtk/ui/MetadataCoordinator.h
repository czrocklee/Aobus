// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "GtkMainThreadDispatcher.h"

#include <ao/Error.h>
#include <ao/Type.h>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::gtk
{
  /**
   * @brief MetadataCoordinator manages asynchronous track metadata updates.
   */
  class MetadataCoordinator final
  {
  public:
    struct MetadataUpdateSpec
    {
      std::optional<std::string> title{};
      std::optional<std::string> artist{};
      std::optional<std::string> album{};
      std::optional<std::string> genre{};
      std::optional<std::string> composer{};
      std::optional<std::string> work{};
      std::vector<std::string> tagsToAdd{};
      std::vector<std::string> tagsToRemove{};
    };

    using UpdateResultCallback = std::function<void(ao::Result<> const&)>;

    explicit MetadataCoordinator(std::shared_ptr<GtkMainThreadDispatcher> dispatcher);
    ~MetadataCoordinator();

    /**
     * @brief Asynchronously updates track metadata.
     */
    void updateMetadata(ao::library::MusicLibrary* library,
                        std::vector<ao::TrackId> ids,
                        MetadataUpdateSpec spec,
                        UpdateResultCallback onResult);

  private:
    struct Task
    {
      ao::library::MusicLibrary* library;
      std::vector<ao::TrackId> trackIds;
      MetadataUpdateSpec spec;
      UpdateResultCallback onResult;
    };

    void workerLoop(std::stop_token stopToken);

    std::shared_ptr<GtkMainThreadDispatcher> _dispatcher;

    std::mutex _queueMutex;
    std::condition_variable _queueCv;
    std::queue<Task> _taskQueue;
    std::jthread _workerThread;
  };
} // namespace ao::gtk
