// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "MetadataCoordinator.h"

#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/utility/Log.h>
#include <ao/utility/ThreadUtils.h>

#include <chrono>

namespace ao::gtk
{
  MetadataCoordinator::MetadataCoordinator(std::shared_ptr<GtkMainThreadDispatcher> dispatcher)
    : _dispatcher{std::move(dispatcher)}
    , _workerThread{[this](std::stop_token stopToken) { workerLoop(std::move(stopToken)); }}
  {
  }

  MetadataCoordinator::~MetadataCoordinator()
  {
    _workerThread.request_stop();
    _queueCv.notify_all();
  }

  void MetadataCoordinator::updateMetadata(ao::library::MusicLibrary* library,
                                           std::vector<ao::TrackId> ids,
                                           MetadataUpdateSpec spec,
                                           UpdateResultCallback onResult)
  {
    {
      auto const lock = std::lock_guard{_queueMutex};
      _taskQueue.push(
        Task{.library = library, .trackIds = std::move(ids), .spec = std::move(spec), .onResult = std::move(onResult)});
    }
    _queueCv.notify_one();
  }

  void MetadataCoordinator::workerLoop(std::stop_token stopToken)
  {
    ao::setCurrentThreadName("MetadataWorker");

    while (!stopToken.stop_requested())
    {
      Task task;
      {
        auto lock = std::unique_lock{_queueMutex};
        _queueCv.wait(lock, [&] { return stopToken.stop_requested() || !_taskQueue.empty(); });

        if (stopToken.stop_requested())
        {
          break;
        }

        task = std::move(_taskQueue.front());
        _taskQueue.pop();
      }

      if (!task.library)
      {
        _dispatcher->dispatch([callback = std::move(task.onResult)]
                              { callback(std::unexpected(ao::Error{ao::Error::Code::Generic, "No active library"})); });
        continue;
      }

      APP_LOG_INFO("MetadataCoordinator: Processing update for {} tracks", task.trackIds.size());

      try
      {
        auto& library = *task.library;
        auto txn = library.writeTransaction();
        auto store = library.tracks().writer(txn);

        for (auto const& trackId : task.trackIds)
        {
          auto const optView = store.get(trackId, ao::library::TrackStore::Reader::LoadMode::Both);
          if (!optView)
          {
            APP_LOG_WARN("MetadataCoordinator: Track {} not found, skipping", trackId.value());
            continue;
          }

          auto builder = ao::library::TrackBuilder::fromView(*optView, library.dictionary());
          bool changedHot = false;
          bool changedCold = false;

          if (task.spec.title)
          {
            builder.metadata().title(*task.spec.title);
            changedHot = true;
          }
          if (task.spec.artist)
          {
            builder.metadata().artist(*task.spec.artist);
            changedHot = true;
          }
          if (task.spec.album)
          {
            builder.metadata().album(*task.spec.album);
            changedHot = true;
          }
          if (task.spec.genre)
          {
            builder.metadata().genre(*task.spec.genre);
            changedHot = true;
          }
          if (task.spec.composer)
          {
            builder.metadata().composer(*task.spec.composer);
            changedHot = true;
          }
          if (task.spec.work)
          {
            builder.metadata().work(*task.spec.work);
            changedCold = true;
          }

          for (auto const& tag : task.spec.tagsToAdd)
          {
            builder.tags().add(tag);
            changedHot = true;
          }

          for (auto const& tag : task.spec.tagsToRemove)
          {
            builder.tags().remove(tag);
            changedHot = true;
          }

          if (changedHot)
          {
            auto const hotData = builder.serializeHot(txn, library.dictionary());
            store.updateHot(trackId, hotData);
          }

          if (changedCold)
          {
            auto const coldData = builder.serializeCold(txn, library.dictionary(), library.resources());
            store.updateCold(trackId, coldData);
          }
        }

        txn.commit();

        _dispatcher->dispatch([callback = std::move(task.onResult)] { callback({}); });
      }
      catch (std::exception const& e)
      {
        APP_LOG_ERROR("MetadataCoordinator: Update failed: {}", e.what());
        _dispatcher->dispatch([callback = std::move(task.onResult), msg = std::string{e.what()}]
                              { callback(std::unexpected(ao::Error{ao::Error::Code::Generic, msg})); });
      }
    }
  }
} // namespace ao::gtk
