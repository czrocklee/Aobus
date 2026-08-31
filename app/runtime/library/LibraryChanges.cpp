// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/LibraryChanges.h>

#include <ao/Contract.h>
#include <ao/async/Executor.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/utility/CallbackStackScope.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::rt
{
  struct LibraryChanges::Impl final
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

    class OwnerLease;

    class DeliveryAdmissionState final : public std::enable_shared_from_this<DeliveryAdmissionState>
    {
    public:
      explicit DeliveryAdmissionState(Impl* owner) noexcept
        : _owner{owner}
      {
      }

      std::optional<OwnerLease> acquire();

      template<typename Operation>
      void invokeIfAlive(Operation&& operation);

      void beginActiveDelivery(std::string_view replicaName)
      {
        auto const lock = std::scoped_lock{_mutex};
        _activeReplicaName = replicaName;
      }

      void endActiveDelivery()
      {
        auto const lock = std::scoped_lock{_mutex};
        _activeReplicaName.clear();
      }

      std::string activeReplicaName() const
      {
        auto const lock = std::scoped_lock{_mutex};
        return _activeReplicaName.empty() ? std::string{"<unbound>"} : _activeReplicaName;
      }

      void retireAndWait() noexcept
      {
        auto lock = std::unique_lock{_mutex};
        AO_EXPECTS(!utility::CallbackStackScope::containsIdentity(this),
                   "LibraryChanges cannot be destroyed from one of its admitted callback stacks");
        _owner = nullptr;
        _callsCompleted.wait(lock, [this] { return _activeCalls == 0; });
      }

    private:
      void releaseCall() noexcept
      {
        bool allCallsCompleted = false;

        {
          auto const lock = std::scoped_lock{_mutex};
          AO_INVARIANT(_activeCalls != 0);
          --_activeCalls;
          allCallsCompleted = _activeCalls == 0;
        }

        if (allCallsCompleted)
        {
          _callsCompleted.notify_all();
        }
      }

      mutable std::mutex _mutex;
      std::condition_variable _callsCompleted;
      Impl* _owner = nullptr;
      std::size_t _activeCalls = 0;
      std::string _activeReplicaName;

      friend class OwnerLease;
    };

    class OwnerLease final
    {
    public:
      ~OwnerLease()
      {
        if (_owner != nullptr)
        {
          _statePtr->releaseCall();
        }
      }

      OwnerLease(OwnerLease const&) = delete;
      OwnerLease& operator=(OwnerLease const&) = delete;
      OwnerLease(OwnerLease&& other) noexcept
        : _statePtr{std::move(other._statePtr)}, _owner{std::exchange(other._owner, nullptr)}
      {
      }
      OwnerLease& operator=(OwnerLease&&) = delete;

      Impl& owner() const noexcept { return *_owner; }

    private:
      OwnerLease(std::shared_ptr<DeliveryAdmissionState> statePtr, Impl& owner) noexcept
        : _statePtr{std::move(statePtr)}, _owner{&owner}
      {
      }

      std::shared_ptr<DeliveryAdmissionState> _statePtr;
      Impl* _owner = nullptr;

      friend class DeliveryAdmissionState;
    };

    static std::string_view replicaNameOf(std::shared_ptr<ReplicaSlot> const& replicaPtr) noexcept
    {
      return replicaPtr == nullptr ? std::string_view{"<unbound>"} : std::string_view{replicaPtr->name};
    }

    Impl(async::Executor& executor, std::uint64_t const lastPublishedRevision, std::string identity)
      : callbackExecutor{executor}
      , libraryIdentity{std::move(identity)}
      , deliveryAdmissionStatePtr{std::make_shared<DeliveryAdmissionState>(this)}
      , expectedRevision{lastPublishedRevision + 1U}
    {
      AO_EXPECTS(!libraryIdentity.empty(), "Library changes require a diagnostic identity");
    }

    ~Impl() { deliveryAdmissionStatePtr->retireAndWait(); }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    async::Subscription bindReplica(std::string name, compat::MoveOnlyFunction<void(LibraryChangeSet const&)> apply)
    {
      {
        auto const lock = std::scoped_lock{mutex};

        AO_EXPECTS(apply, "Library change replica '{}' requires an apply callback", name);
        AO_EXPECTS(!publicationInProgress, "Cannot bind library change replica '{}' during active publication", name);
        AO_EXPECTS(!replicaSlotPtr, "Library change replica '{}' is already bound", name);

        replicaSlotPtr = std::make_shared<ReplicaSlot>(std::move(name), std::move(apply));
      }

      auto const weakDeliveryStatePtr = std::weak_ptr<DeliveryAdmissionState>{deliveryAdmissionStatePtr};
      return async::Subscription{
        [weakDeliveryStatePtr]
        {
          if (auto const deliveryStatePtr = weakDeliveryStatePtr.lock(); deliveryStatePtr != nullptr)
          {
            deliveryStatePtr->invokeIfAlive([](Impl& owner) noexcept { owner.unbindReplica(); });
          }
        }};
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

        auto const weakDeliveryStatePtr = std::weak_ptr<DeliveryAdmissionState>{deliveryAdmissionStatePtr};
        callbackExecutor.dispatch(
          [weakDeliveryStatePtr] noexcept
          {
            if (auto const deliveryStatePtr = weakDeliveryStatePtr.lock(); deliveryStatePtr != nullptr)
            {
              deliveryStatePtr->invokeIfAlive([](Impl& owner) noexcept { owner.deliverPending(); });
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
      auto replicaName = std::string{};
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
        replicaName = std::string{replicaNameOf(pinnedReplicaPtr)};
        AO_INVARIANT(publicationInProgress,
                     "library publication failure: phase=delivery library='{}' revision={} replica='{}': "
                     "pending delivery lost its publication gate",
                     libraryIdentity,
                     optPending->changeSet.libraryRevision,
                     replicaName);
      }

      deliveryAdmissionStatePtr->beginActiveDelivery(replicaName);
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
                        replicaName));
        }
      }

      // Phase two. Reaching an observer states that the replica is current.
      changedSignal.emit(optPending->changeSet);
      deliveryInProgress.store(false, std::memory_order_release);
      deliveryAdmissionStatePtr->endActiveDelivery();

      {
        auto const lock = std::scoped_lock{mutex};
        publicationInProgress = false;
        ++expectedRevision;
      }

      if (optPending->completion)
      {
        try
        {
          optPending->completion(
            detail::LibraryPublicationTerminal::Published, std::string{libraryIdentity}, replicaName);
        }
        catch (...)
        {
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
      auto const weakDeliveryStatePtr = std::weak_ptr<DeliveryAdmissionState>{deliveryAdmissionStatePtr};
      auto const diagnosticLibraryIdentity = std::string{libraryIdentity};

      return changedSignal.connect(
        [weakDeliveryStatePtr,
         diagnosticLibraryIdentity = std::move(diagnosticLibraryIdentity),
         handler = std::move(handler)](LibraryChangeSet const& changeSet) mutable noexcept
        {
          try
          {
            handler(changeSet);
          }
          catch (...)
          {
            auto const deliveryStatePtr = weakDeliveryStatePtr.lock();
            AO_INVARIANT(deliveryStatePtr != nullptr, "Library observer outlived its publication owner");
            AO_FATAL_EXCEPTION(
              std::current_exception(),
              std::format("library publication failure: phase=observer-delivery library='{}' revision={} replica='{}'",
                          diagnosticLibraryIdentity,
                          changeSet.libraryRevision,
                          deliveryStatePtr->activeReplicaName()));
          }
        });
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
    std::shared_ptr<DeliveryAdmissionState> deliveryAdmissionStatePtr;
    std::shared_ptr<ReplicaSlot> replicaSlotPtr;
    async::Signal<LibraryChangeSet const&> changedSignal;
    mutable std::mutex mutex;
    std::optional<PendingPublication> optPendingPublication;
    std::uint64_t expectedRevision = 1;
    std::atomic_bool deliveryInProgress = false;
    bool publicationInProgress = false;
    bool closing = false;
  };

  std::optional<LibraryChanges::Impl::OwnerLease> LibraryChanges::Impl::DeliveryAdmissionState::acquire()
  {
    auto statePtr = shared_from_this();
    auto const lock = std::scoped_lock{_mutex};

    if (_owner == nullptr)
    {
      return std::nullopt;
    }

    ++_activeCalls;
    return OwnerLease{std::move(statePtr), *_owner};
  }

  template<typename Operation>
  void LibraryChanges::Impl::DeliveryAdmissionState::invokeIfAlive(Operation&& operation)
  {
    if (auto optOwnerLease = acquire(); optOwnerLease)
    {
      auto const invocationScope = utility::CallbackStackScope{this};
      std::invoke(std::forward<Operation>(operation), optOwnerLease->owner());
    }
  }

  LibraryChanges::LibraryChanges(async::Executor& callbackExecutor,
                                 std::uint64_t lastPublishedRevision,
                                 std::string libraryIdentity)
    : _implPtr{std::make_unique<Impl>(callbackExecutor, lastPublishedRevision, std::move(libraryIdentity))}
  {
  }

  LibraryChanges::~LibraryChanges() = default;

  async::Subscription LibraryChanges::bindReplica(std::string replicaName,
                                                  compat::MoveOnlyFunction<void(LibraryChangeSet const&)> apply) const
  {
    return _implPtr->bindReplica(std::move(replicaName), std::move(apply));
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
