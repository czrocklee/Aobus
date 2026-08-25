// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/LibraryChanges.h>

#include <ao/Contract.h>
#include <ao/async/Executor.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/compat/MoveOnlyFunction.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::rt
{
  struct LibraryChanges::Impl final : std::enable_shared_from_this<Impl>
  {
    struct PendingPublication final
    {
      LibraryChangeSet changeSet{};
      compat::MoveOnlyFunction<
        void(detail::LibraryPublicationTerminal terminal, std::string libraryIdentity, std::string replicaName)>
        completion{};
    };

    struct ReplicaSlot final
    {
      std::string name{};
      compat::MoveOnlyFunction<void(LibraryChangeSet const&)> apply{};
    };

    static std::string_view replicaNameOf(std::shared_ptr<ReplicaSlot> const& replicaPtr) noexcept
    {
      return replicaPtr == nullptr ? std::string_view{"<unbound>"} : std::string_view{replicaPtr->name};
    }

    Impl(async::Executor& executor, std::uint64_t const lastPublishedRevision, std::string identity)
      : callbackExecutor{executor}, libraryIdentity{std::move(identity)}, expectedRevision{lastPublishedRevision + 1U}
    {
      AO_EXPECTS(!libraryIdentity.empty(), "Library changes require a diagnostic identity");
    }

    void bindReplica(std::string name, compat::MoveOnlyFunction<void(LibraryChangeSet const&)> apply)
    {
      auto const lock = std::scoped_lock{mutex};

      AO_EXPECTS(apply, "Library change replica '{}' requires an apply callback", name);
      AO_EXPECTS(!publicationInProgress, "Cannot bind library change replica '{}' during active publication", name);
      AO_EXPECTS(!replicaSlotPtr, "Library change replica '{}' is already bound", name);

      replicaSlotPtr = std::make_shared<ReplicaSlot>(std::move(name), std::move(apply));
    }

    void unbindReplica()
    {
      auto const lock = std::scoped_lock{mutex};
      replicaSlotPtr.reset();
    }

    void publish(LibraryChangeSet changeSet,
                 compat::MoveOnlyFunction<void(detail::LibraryPublicationTerminal terminal,
                                               std::string libraryIdentity,
                                               std::string replicaName)> completion) noexcept
    {
      auto const revision = changeSet.libraryRevision;

      try
      {
        {
          auto const lock = std::scoped_lock{mutex};
          auto const replicaName = replicaNameOf(replicaSlotPtr);

          AO_INVARIANT(revision != 0,
                       "library publication failure: phase=admission library='{}' revision={} replica='{}': "
                       "changeset revision must be nonzero",
                       libraryIdentity,
                       revision,
                       replicaName);
          AO_INVARIANT(revision == expectedRevision,
                       "library publication failure: phase=admission library='{}' revision={} replica='{}': "
                       "expected revision {}",
                       libraryIdentity,
                       revision,
                       replicaName,
                       expectedRevision);
          AO_INVARIANT(!publicationInProgress,
                       "library publication failure: phase=admission library='{}' revision={} replica='{}': "
                       "another publication is active",
                       libraryIdentity,
                       revision,
                       replicaName);
          AO_INVARIANT(!closing,
                       "library publication failure: phase=admission library='{}' revision={} replica='{}': "
                       "publication owner is closing",
                       libraryIdentity,
                       revision,
                       replicaName);

          optPendingPublication.emplace(
            PendingPublication{.changeSet = std::move(changeSet), .completion = std::move(completion)});
          publicationInProgress = true;
        }

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
        auto pinnedReplicaPtr = std::shared_ptr<ReplicaSlot>{};
        {
          auto const lock = std::scoped_lock{mutex};
          pinnedReplicaPtr = replicaSlotPtr;
        }

        auto const replicaName = replicaNameOf(pinnedReplicaPtr);
        AO_FATAL_EXCEPTION(
          std::current_exception(),
          std::format("library publication failure: phase=admission library='{}' revision={} replica='{}'",
                      libraryIdentity,
                      revision,
                      replicaName));
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

        optPending.emplace(std::move(*optPendingPublication));
        optPendingPublication.reset();
        // Pinned under the lock, applied outside it, so binding cannot race a
        // delivery already in flight.
        pinnedReplicaPtr = replicaSlotPtr;
        activeReplicaPtr = pinnedReplicaPtr;
        auto const replicaName = replicaNameOf(pinnedReplicaPtr);
        AO_INVARIANT(publicationInProgress,
                     "library publication failure: phase=delivery library='{}' revision={} replica='{}': "
                     "pending delivery lost its publication gate",
                     libraryIdentity,
                     optPending->changeSet.libraryRevision,
                     replicaName);
      }

      deliveryInProgress.store(true, std::memory_order_release);

      if (pinnedReplicaPtr)
      {
        try
        {
          pinnedReplicaPtr->apply(optPending->changeSet);
        }
        catch (...)
        {
          AO_FATAL_EXCEPTION(
            std::current_exception(),
            std::format("library publication failure: phase=replica-apply library='{}' revision={} replica='{}'",
                        libraryIdentity,
                        optPending->changeSet.libraryRevision,
                        pinnedReplicaPtr->name));
        }
      }

      // Phase two. Reaching an observer states that the replica is current.
      changedSignal.emit(optPending->changeSet);
      deliveryInProgress.store(false, std::memory_order_release);

      {
        auto const lock = std::scoped_lock{mutex};
        activeReplicaPtr.reset();
        publicationInProgress = false;
        ++expectedRevision;
      }

      if (optPending->completion)
      {
        try
        {
          optPending->completion(detail::LibraryPublicationTerminal::Published,
                                 std::string{libraryIdentity},
                                 std::string{replicaNameOf(pinnedReplicaPtr)});
        }
        catch (...)
        {
          auto const replicaName = replicaNameOf(pinnedReplicaPtr);
          AO_FATAL_EXCEPTION(
            std::current_exception(),
            std::format("library publication failure: phase=completion library='{}' revision={} replica='{}'",
                        libraryIdentity,
                        optPending->changeSet.libraryRevision,
                        replicaName));
        }
      }
    }

    async::Subscription connectObserver(compat::MoveOnlyFunction<void(LibraryChangeSet const&)> handler)
    {
      return changedSignal.connect(
        [weakImplPtr = weak_from_this(),
         handler = std::move(handler)](LibraryChangeSet const& changeSet) mutable noexcept
        {
          try
          {
            handler(changeSet);
          }
          catch (...)
          {
            auto const lockedPtr = weakImplPtr.lock();
            AO_INVARIANT(lockedPtr != nullptr, "Library observer outlived its publication owner");
            auto const pinnedReplicaPtr = lockedPtr->activeReplica();
            auto const replicaName = replicaNameOf(pinnedReplicaPtr);
            AO_FATAL_EXCEPTION(
              std::current_exception(),
              std::format("library publication failure: phase=observer-delivery library='{}' revision={} replica='{}'",
                          lockedPtr->libraryIdentity,
                          changeSet.libraryRevision,
                          replicaName));
          }
        });
    }

    std::shared_ptr<ReplicaSlot> activeReplica() const
    {
      auto const lock = std::scoped_lock{mutex};
      return activeReplicaPtr;
    }

    bool isDeliveryInProgress() const noexcept { return deliveryInProgress.load(std::memory_order_acquire); }

    void sealAndRetire() noexcept
    {
      auto retiredCompletion =
        compat::MoveOnlyFunction<void(detail::LibraryPublicationTerminal, std::string, std::string)>{};

      {
        auto const lock = std::scoped_lock{mutex};
        closing = true;

        if (optPendingPublication)
        {
          retiredCompletion = std::move(optPendingPublication->completion);
          optPendingPublication.reset();
          publicationInProgress = false;
        }
      }

      if (retiredCompletion)
      {
        retiredCompletion(detail::LibraryPublicationTerminal::RetiredByClosing, {}, {});
      }
    }

    async::Executor& callbackExecutor;
    std::string const libraryIdentity;
    std::shared_ptr<ReplicaSlot> replicaSlotPtr;
    std::shared_ptr<ReplicaSlot> activeReplicaPtr;
    async::Signal<LibraryChangeSet const&> changedSignal;
    mutable std::mutex mutex;
    std::optional<PendingPublication> optPendingPublication;
    std::uint64_t expectedRevision = 1;
    std::atomic_bool deliveryInProgress = false;
    bool publicationInProgress = false;
    bool closing = false;
  };

  LibraryChanges::LibraryChanges(async::Executor& callbackExecutor,
                                 std::uint64_t lastPublishedRevision,
                                 std::string libraryIdentity)
    : _implPtr{std::make_shared<Impl>(callbackExecutor, lastPublishedRevision, std::move(libraryIdentity))}
  {
  }

  LibraryChanges::~LibraryChanges() = default;

  async::Subscription LibraryChanges::bindReplica(std::string replicaName,
                                                  compat::MoveOnlyFunction<void(LibraryChangeSet const&)> apply) const
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

  async::Subscription LibraryChanges::onChanged(compat::MoveOnlyFunction<void(LibraryChangeSet const&)> handler) const
  {
    return _implPtr->connectObserver(std::move(handler));
  }

  void LibraryChanges::publishFromCoordinator(
    LibraryChangeSet changeSet,
    compat::MoveOnlyFunction<void(detail::LibraryPublicationTerminal terminal,
                                  std::string libraryIdentity,
                                  std::string replicaName)> completion) noexcept
  {
    _implPtr->publish(std::move(changeSet), std::move(completion));
  }

  bool LibraryChanges::publicationDeliveryInProgressFromCoordinator() const noexcept
  {
    return _implPtr->isDeliveryInProgress();
  }

  void LibraryChanges::sealAndRetireFromCoordinator() noexcept
  {
    _implPtr->sealAndRetire();
  }
} // namespace ao::rt
