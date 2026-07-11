// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Exception.h>
#include <ao/async/Executor.h>
#include <ao/rt/Signal.h>
#include <ao/rt/Subscription.h>
#include <ao/rt/library/LibraryChanges.h>

#include <gsl-lite/gsl-lite.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace ao::rt
{
  struct LibraryChanges::Impl final
  {
    explicit Impl(async::Executor* executor = nullptr, std::uint64_t lastPublishedRevision = 0)
      : callbackExecutor{executor}
    {
      if (lastPublishedRevision != 0)
      {
        optNextRevision = lastPublishedRevision + 1U;
      }
    }

    void publish(LibraryChangeSet changeSet)
    {
      if (changeSet.libraryRevision == 0)
      {
        throwException<Exception>("Library changeset must carry a non-zero revision");
      }

      bool schedule = false;
      {
        auto const lock = std::scoped_lock{mutex};

        if (!optNextRevision || (callbackExecutor == nullptr && changeSet.libraryRevision > *optNextRevision))
        {
          optNextRevision = changeSet.libraryRevision;
        }

        gsl_Assert(optNextRevision);

        if (changeSet.libraryRevision < *optNextRevision || holdback.contains(changeSet.libraryRevision))
        {
          throwException<Exception>("Duplicate or stale library changeset revision {}", changeSet.libraryRevision);
        }

        holdback.emplace(changeSet.libraryRevision, std::move(changeSet));

        if (!drainScheduled)
        {
          drainScheduled = true;
          schedule = true;
        }
      }

      if (!schedule)
      {
        return;
      }

      if (callbackExecutor != nullptr)
      {
        callbackExecutor->dispatch([this] { drain(); });
      }
      else
      {
        drain();
      }
    }

    void drain()
    {
      auto failure = std::exception_ptr{};

      while (true)
      {
        auto changeSetNode = decltype(holdback)::node_type{};
        {
          auto const lock = std::scoped_lock{mutex};
          gsl_Assert(optNextRevision);
          auto const it = holdback.find(*optNextRevision);

          if (it == holdback.end())
          {
            drainScheduled = false;

            if (failure)
            {
              std::rethrow_exception(failure);
            }

            return;
          }

          changeSetNode = holdback.extract(it);
          ++*optNextRevision;
        }

        try
        {
          changedSignal.emit(changeSetNode.mapped());
        }
        catch (...)
        {
          if (!failure)
          {
            failure = std::current_exception();
          }
        }
      }
    }

    async::Executor* callbackExecutor = nullptr;
    Signal<LibraryChangeSet const&> changedSignal;
    Signal<std::size_t> libraryTaskCompletedSignal;
    Signal<LibraryChanges::LibraryTaskProgressUpdated const&> libraryTaskProgressSignal;
    std::mutex mutex;
    std::map<std::uint64_t, LibraryChangeSet> holdback;
    std::optional<std::uint64_t> optNextRevision;
    bool drainScheduled = false;
  };

  LibraryChanges::LibraryChanges()
    : _implPtr{std::make_unique<Impl>()}
  {
  }

  LibraryChanges::LibraryChanges(async::Executor& callbackExecutor, std::uint64_t lastPublishedRevision)
    : _implPtr{std::make_unique<Impl>(&callbackExecutor, lastPublishedRevision)}
  {
  }

  LibraryChanges::~LibraryChanges() = default;

  Subscription LibraryChanges::onChanged(std::move_only_function<void(LibraryChangeSet const&)> handler) const
  {
    return _implPtr->changedSignal.connect(std::move(handler));
  }

  Subscription LibraryChanges::onLibraryTaskCompleted(std::move_only_function<void(std::size_t)> handler) const
  {
    return _implPtr->libraryTaskCompletedSignal.connect(std::move(handler));
  }

  Subscription LibraryChanges::onLibraryTaskProgress(
    std::move_only_function<void(LibraryTaskProgressUpdated const&)> handler) const
  {
    return _implPtr->libraryTaskProgressSignal.connect(std::move(handler));
  }

  void LibraryChanges::publish(LibraryChangeSet changeSet)
  {
    _implPtr->publish(std::move(changeSet));
  }

  void LibraryChanges::notifyLibraryTaskProgress(LibraryTaskProgressUpdated progress)
  {
    _implPtr->libraryTaskProgressSignal.emit(progress);
  }

  void LibraryChanges::notifyLibraryTaskCompleted(std::size_t count)
  {
    _implPtr->libraryTaskCompletedSignal.emit(count);
  }
} // namespace ao::rt
