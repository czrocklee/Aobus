// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/LibraryChanges.h>

#include <ao/Exception.h>
#include <ao/ExceptionFormat.h>
#include <ao/async/Executor.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>

#include <gsl-lite/gsl-lite.hpp>

#include <cstdint>
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
      std::move_only_function<void() noexcept> completion{};
    };

    struct ReplicaSlot final
    {
      std::string name{};
      std::move_only_function<void(LibraryChangeSet const&) noexcept> apply{};
    };

    Impl(async::Executor& executor, std::uint64_t const lastPublishedRevision)
      : callbackExecutor{executor}, expectedRevision{lastPublishedRevision + 1U}
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

    void publish(LibraryChangeSet changeSet, std::move_only_function<void() noexcept> completion)
    {
      if (changeSet.libraryRevision == 0)
      {
        throwException<Exception>("Library changeset must carry a non-zero revision");
      }

      auto const revision = changeSet.libraryRevision;
      {
        auto const lock = std::scoped_lock{mutex};

        if (revision != expectedRevision)
        {
          throwException<Exception>(
            "Out-of-sequence library changeset revision: expected {}, got {}", expectedRevision, revision);
        }

        if (publicationInProgress)
        {
          throwException<Exception>("Library changeset revision {} submitted during active publication", revision);
        }

        if (closing)
        {
          throwException<Exception>("Library changeset revision {} submitted while closing", revision);
        }

        optPendingPublication.emplace(
          PendingPublication{.changeSet = std::move(changeSet), .completion = std::move(completion)});
        publicationInProgress = true;
      }

      try
      {
        callbackExecutor.dispatch(
          [weakImplPtr = weak_from_this()] noexcept
          {
            if (auto const lockedPtr = weakImplPtr.lock(); lockedPtr != nullptr)
            {
              lockedPtr->deliverPending();
            }
          });
      }
      catch (...)
      {
        {
          auto const lock = std::scoped_lock{mutex};

          // A synchronous delivery extracts the pending value before running.
          // If none remains, its failure is already complete and only needs to
          // propagate. A still-pending value means dispatch rejected the task.
          if (optPendingPublication)
          {
            optPendingPublication.reset();
            publicationInProgress = false;
          }
        }

        throw;
      }
    }

    void deliverPending() noexcept
    {
      auto optPending = std::optional<PendingPublication>{};
      auto pinnedReplicaPtr = std::shared_ptr<ReplicaSlot>{};
      {
        auto const lock = std::scoped_lock{mutex};

        if (!optPendingPublication)
        {
          return;
        }

        gsl_Assert(publicationInProgress);
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
        ++expectedRevision;
      }

      if (optPending->completion)
      {
        optPending->completion();
      }
    }

    void retire() noexcept
    {
      auto const lock = std::scoped_lock{mutex};
      closing = true;

      if (optPendingPublication)
      {
        optPendingPublication.reset();
        publicationInProgress = false;
      }
    }

    async::Executor& callbackExecutor;
    std::shared_ptr<ReplicaSlot> replicaSlotPtr;
    async::Signal<LibraryChangeSet const&> changedSignal;
    std::mutex mutex;
    std::optional<PendingPublication> optPendingPublication;
    std::uint64_t expectedRevision = 1;
    bool publicationInProgress = false;
    bool closing = false;
  };

  LibraryChanges::LibraryChanges(async::Executor& callbackExecutor, std::uint64_t lastPublishedRevision)
    : _implPtr{std::make_shared<Impl>(callbackExecutor, lastPublishedRevision)}
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

  void LibraryChanges::publishFromCoordinator(LibraryChangeSet changeSet,
                                              std::move_only_function<void() noexcept> completion)
  {
    _implPtr->publish(std::move(changeSet), std::move(completion));
  }

  void LibraryChanges::retireFromCoordinator() noexcept
  {
    _implPtr->retire();
  }
} // namespace ao::rt
