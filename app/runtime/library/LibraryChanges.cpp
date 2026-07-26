// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Exception.h>
#include <ao/async/Executor.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/rt/library/LibraryChanges.h>

#include <gsl-lite/gsl-lite.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace ao::rt
{
  struct LibraryChanges::Impl final : std::enable_shared_from_this<Impl>
  {
    struct PendingPublication final
    {
      LibraryChangeSet changeSet{};
      std::move_only_function<void(std::exception_ptr)> completion{};
    };

    struct ReplicaSlot final
    {
      std::string name{};
      std::move_only_function<void(LibraryChangeSet const&) noexcept> apply{};
    };

    Impl() = default;

    Impl(async::Executor* executor, std::uint64_t lastPublishedRevision)
      : callbackExecutor{executor}, optExpectedRevision{lastPublishedRevision + 1U}
    {
    }

    void bindReplica(std::string name, std::move_only_function<void(LibraryChangeSet const&) noexcept> apply)
    {
      auto const lock = std::scoped_lock{mutex};

      if (!apply)
      {
        throwException<Exception>("Library change replica '{}' requires an apply callback", name);
      }

      if (publicationInProgress)
      {
        throwException<Exception>("Cannot bind library change replica '{}' during active publication", name);
      }

      if (replicaSlotPtr)
      {
        throwException<Exception>("Library change replica '{}' is already bound", replicaSlotPtr->name);
      }

      replicaSlotPtr = std::make_shared<ReplicaSlot>(std::move(name), std::move(apply));
    }

    void unbindReplica()
    {
      auto const lock = std::scoped_lock{mutex};
      replicaSlotPtr.reset();
    }

    void publish(LibraryChangeSet changeSet, std::move_only_function<void(std::exception_ptr)> completion = {})
    {
      if (changeSet.libraryRevision == 0)
      {
        throwException<Exception>("Library changeset must carry a non-zero revision");
      }

      auto const revision = changeSet.libraryRevision;
      {
        auto const lock = std::scoped_lock{mutex};

        // The synchronous test bus has no persisted baseline, so its first
        // submission establishes one. Production construction supplies the
        // last published revision and therefore checks the first submission too.
        if (!optExpectedRevision)
        {
          optExpectedRevision = revision;
        }

        gsl_Assert(optExpectedRevision);

        if (revision != *optExpectedRevision)
        {
          throwException<Exception>(
            "Out-of-sequence library changeset revision: expected {}, got {}", *optExpectedRevision, revision);
        }

        if (publicationInProgress)
        {
          throwException<Exception>("Library changeset revision {} submitted during active publication", revision);
        }

        optPendingPublication.emplace(
          PendingPublication{.changeSet = std::move(changeSet), .completion = std::move(completion)});
        publicationInProgress = true;
      }

      try
      {
        if (callbackExecutor != nullptr)
        {
          callbackExecutor->dispatch(
            [weakImplPtr = weak_from_this()]
            {
              if (auto const lockedPtr = weakImplPtr.lock(); lockedPtr != nullptr)
              {
                lockedPtr->deliverPending();
              }
            });
        }
        else
        {
          deliverPending();
        }
      }
      catch (...)
      {
        auto optFailedPublication = std::optional<PendingPublication>{};

        {
          auto const lock = std::scoped_lock{mutex};

          // A synchronous delivery extracts the pending value before running.
          // If none remains, its failure is already complete and only needs to
          // propagate. A still-pending value means dispatch rejected the task.
          if (optPendingPublication)
          {
            optFailedPublication.emplace(std::move(*optPendingPublication));
            optPendingPublication.reset();
            publicationInProgress = false;
          }
        }

        auto const failure = std::current_exception();

        if (optFailedPublication && optFailedPublication->completion)
        {
          try
          {
            optFailedPublication->completion(failure);
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

    void deliverPending()
    {
      auto optPending = std::optional<PendingPublication>{};
      auto pinnedReplicaPtr = std::shared_ptr<ReplicaSlot>{};
      {
        auto const lock = std::scoped_lock{mutex};
        gsl_Assert(publicationInProgress);
        gsl_Assert(optPendingPublication);
        optPending.emplace(std::move(*optPendingPublication));
        optPendingPublication.reset();
        // Pinned under the lock, applied outside it, so binding cannot race a
        // delivery already in flight.
        pinnedReplicaPtr = replicaSlotPtr;
      }

      if (pinnedReplicaPtr)
      {
        pinnedReplicaPtr->apply(optPending->changeSet);
      }

      // Phase two. Reaching an observer states that the replica is current.
      changedSignal.emit(optPending->changeSet);

      {
        auto const lock = std::scoped_lock{mutex};
        publicationInProgress = false;
        gsl_Assert(optExpectedRevision);
        ++*optExpectedRevision;
      }

      if (optPending->completion)
      {
        optPending->completion({});
      }
    }

    async::Executor* callbackExecutor = nullptr;
    std::shared_ptr<ReplicaSlot> replicaSlotPtr;
    async::Signal<LibraryChangeSet const&> changedSignal;
    async::Signal<LibraryChanges::LibraryTaskCompleted const&> libraryTaskCompletedSignal;
    async::Signal<LibraryChanges::LibraryTaskProgressUpdated const&> libraryTaskProgressSignal;
    std::mutex mutex;
    std::optional<PendingPublication> optPendingPublication;
    std::optional<std::uint64_t> optExpectedRevision;
    bool publicationInProgress = false;
  };

  LibraryChanges::LibraryChanges()
    : _implPtr{std::make_shared<Impl>()}
  {
  }

  LibraryChanges::LibraryChanges(async::Executor& callbackExecutor, std::uint64_t lastPublishedRevision)
    : _implPtr{std::make_shared<Impl>(&callbackExecutor, lastPublishedRevision)}
  {
  }

  LibraryChanges::~LibraryChanges() = default;

  async::Subscription LibraryChanges::bindReplica(
    std::string replicaName,
    std::move_only_function<void(LibraryChangeSet const&) noexcept> apply) const
  {
    _implPtr->bindReplica(std::move(replicaName), std::move(apply));

    return async::Subscription{[weakImplPtr = std::weak_ptr<Impl>{_implPtr}]
                               {
                                 if (auto const lockedPtr = weakImplPtr.lock(); lockedPtr != nullptr)
                                 {
                                   lockedPtr->unbindReplica();
                                 }
                               }};
  }

  async::Subscription LibraryChanges::onChanged(
    std::move_only_function<void(LibraryChangeSet const&) noexcept> handler) const
  {
    return _implPtr->changedSignal.connect(std::move(handler));
  }

  async::Subscription LibraryChanges::onLibraryTaskCompleted(
    std::move_only_function<void(LibraryTaskCompleted const&) noexcept> handler) const
  {
    return _implPtr->libraryTaskCompletedSignal.connect(std::move(handler));
  }

  async::Subscription LibraryChanges::onLibraryTaskProgress(
    std::move_only_function<void(LibraryTaskProgressUpdated const&) noexcept> handler) const
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
