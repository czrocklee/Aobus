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
    struct PendingPublication final
    {
      LibraryChangeSet changeSet{};
      std::move_only_function<void(std::exception_ptr)> completion{};
    };

    explicit Impl(async::Executor* executor = nullptr, std::uint64_t lastPublishedRevision = 0)
      : callbackExecutor{executor}
    {
      if (lastPublishedRevision != 0)
      {
        optNextRevision = lastPublishedRevision + 1U;
      }
    }

    void publish(LibraryChangeSet changeSet, std::move_only_function<void(std::exception_ptr)> completion = {})
    {
      if (changeSet.libraryRevision == 0)
      {
        throwException<Exception>("Library changeset must carry a non-zero revision");
      }

      auto const revision = changeSet.libraryRevision;
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

        holdback.emplace(
          revision, PendingPublication{.changeSet = std::move(changeSet), .completion = std::move(completion)});

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

      try
      {
        if (callbackExecutor != nullptr)
        {
          callbackExecutor->dispatch([this] { drain(); });
        }
        else
        {
          drain();
        }
      }
      catch (...)
      {
        auto failedCompletion = std::move_only_function<void(std::exception_ptr)>{};

        {
          auto const lock = std::scoped_lock{mutex};

          if (auto const it = holdback.find(revision); it != holdback.end())
          {
            failedCompletion = std::move(it->second.completion);
            holdback.erase(it);
          }

          drainScheduled = false;
        }

        auto const failure = std::current_exception();

        if (failedCompletion)
        {
          try
          {
            failedCompletion(failure);
          }
          catch (...)
          {
            // Preserve the publication failure; completion is fault cleanup.
            std::rethrow_exception(failure);
          }
        }

        std::rethrow_exception(failure);
      }
    }

    void drain()
    {
      auto firstFailure = std::exception_ptr{};

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

            if (firstFailure)
            {
              std::rethrow_exception(firstFailure);
            }

            return;
          }

          changeSetNode = holdback.extract(it);
          ++*optNextRevision;
        }

        auto publicationFailure = std::exception_ptr{};

        try
        {
          changedSignal.emit(changeSetNode.mapped().changeSet);
        }
        catch (...)
        {
          publicationFailure = std::current_exception();
        }

        if (changeSetNode.mapped().completion)
        {
          try
          {
            changeSetNode.mapped().completion(publicationFailure);
          }
          catch (...)
          {
            if (!publicationFailure)
            {
              publicationFailure = std::current_exception();
            }
          }
        }

        if (publicationFailure && !firstFailure)
        {
          firstFailure = std::move(publicationFailure);
        }
      }
    }

    async::Executor* callbackExecutor = nullptr;
    Signal<LibraryChangeSet const&> changedSignal;
    Signal<LibraryChanges::LibraryTaskCompleted const&> libraryTaskCompletedSignal;
    Signal<LibraryChanges::LibraryTaskProgressUpdated const&> libraryTaskProgressSignal;
    std::mutex mutex;
    std::map<std::uint64_t, PendingPublication> holdback;
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

  Subscription LibraryChanges::onLibraryTaskCompleted(
    std::move_only_function<void(LibraryTaskCompleted const&)> handler) const
  {
    return _implPtr->libraryTaskCompletedSignal.connect(std::move(handler));
  }

  Subscription LibraryChanges::onLibraryTaskProgress(
    std::move_only_function<void(LibraryTaskProgressUpdated const&)> handler) const
  {
    return _implPtr->libraryTaskProgressSignal.connect(std::move(handler));
  }

  void LibraryChanges::publishFromCoordinator(LibraryChangeSet changeSet,
                                              std::move_only_function<void(std::exception_ptr)> completion)
  {
    _implPtr->publish(std::move(changeSet), std::move(completion));
  }

  void LibraryChanges::notifyLibraryTaskProgress(LibraryTaskProgressUpdated progress)
  {
    _implPtr->libraryTaskProgressSignal.emit(progress);
  }

  void LibraryChanges::notifyLibraryTaskCompleted(LibraryTaskCompletionStatus const status,
                                                  std::size_t const affectedCount)
  {
    auto const event = LibraryTaskCompleted{.status = status, .affectedCount = affectedCount};
    _implPtr->libraryTaskCompletedSignal.emit(event);
  }
} // namespace ao::rt
