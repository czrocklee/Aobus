// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "backend/detail/BackendDeviceRegistry.h"

#include "backend/detail/BackendObserverDispatcher.h"
#include "backend/detail/BackendObserverSlot.h"
#include <ao/Contract.h>
#include <ao/audio/Device.h>
#include <ao/utility/ScopedRegistration.h>

#include <exception>
#include <memory>
#include <mutex>
#include <stop_token>
#include <utility>
#include <vector>

namespace ao::audio::backend::detail
{
  struct BackendDeviceRegistry::State final
  {
    using Slot = BackendObserverSlot<Callback>;

    explicit State(std::shared_ptr<BackendObserverDispatcher> observerDispatcherPtr)
      : dispatcherPtr{std::move(observerDispatcherPtr)}
    {
      AO_INVARIANT(dispatcherPtr);
    }

    void deliver(Slot& slot, std::vector<Device> const& snapshot)
    {
      try
      {
        calls.invoke([&] { slot.invoke([&](Callback const& callback) { callback(snapshot); }); });
      }
      catch (...)
      {
        AO_FATAL_EXCEPTION(std::current_exception(), "audio backend device observer");
      }
    }

    std::shared_ptr<BackendObserverDispatcher> dispatcherPtr;
    BackendObserverCalls calls;
    std::mutex mutex;
    std::shared_ptr<std::vector<Device> const> devicesPtr = std::make_shared<std::vector<Device> const>();
    std::vector<std::shared_ptr<Slot>> subscribers;
    bool shutdown = false;
  };

  BackendDeviceRegistry::BackendDeviceRegistry()
    : BackendDeviceRegistry{std::make_shared<BackendObserverDispatcher>()}
  {
  }

  BackendDeviceRegistry::BackendDeviceRegistry(std::shared_ptr<BackendObserverDispatcher> dispatcherPtr)
    : _statePtr{std::make_shared<State>(std::move(dispatcherPtr))}
  {
  }

  BackendDeviceRegistry::~BackendDeviceRegistry()
  {
    shutdown();
  }

  utility::ScopedRegistration BackendDeviceRegistry::subscribe(Callback callback)
  {
    if (!callback)
    {
      return {};
    }

    auto const statePtr = _statePtr;
    auto const slotPtr = std::make_shared<State::Slot>(std::move(callback));
    statePtr->dispatcherPtr->dispatch(
      [&]
      {
        auto snapshotPtr = std::shared_ptr<std::vector<Device> const>{};
        {
          auto const lock = std::scoped_lock{statePtr->mutex};

          if (statePtr->shutdown)
          {
            return;
          }

          snapshotPtr = statePtr->devicesPtr;
          statePtr->subscribers.push_back(slotPtr);
        }
        statePtr->deliver(*slotPtr, *snapshotPtr);
      });

    {
      auto const lock = std::scoped_lock{statePtr->mutex};

      if (statePtr->shutdown)
      {
        return {};
      }
    }

    return utility::ScopedRegistration{[weakStatePtr = std::weak_ptr{statePtr}, weakSlotPtr = std::weak_ptr{slotPtr}]
                                       {
                                         auto const slotPtr = weakSlotPtr.lock();

                                         if (!slotPtr)
                                         {
                                           return;
                                         }

                                         slotPtr->retire();

                                         if (auto const statePtr = weakStatePtr.lock(); statePtr)
                                         {
                                           auto const lock = std::scoped_lock{statePtr->mutex};
                                           std::erase(statePtr->subscribers, slotPtr);
                                         }
                                       }};
  }

  std::vector<Device> BackendDeviceRegistry::snapshot() const
  {
    auto const statePtr = _statePtr;
    auto const lock = std::scoped_lock{statePtr->mutex};
    return statePtr->devicesPtr ? *statePtr->devicesPtr : std::vector<Device>{};
  }

  void BackendDeviceRegistry::publish(std::vector<Device> devices, std::stop_token stop)
  {
    auto const statePtr = _statePtr;
    auto const snapshotPtr = std::make_shared<std::vector<Device> const>(std::move(devices));
    auto subscribers = std::vector<std::weak_ptr<State::Slot>>{};
    {
      auto const lock = std::scoped_lock{statePtr->mutex};

      if (statePtr->shutdown)
      {
        return;
      }

      statePtr->devicesPtr = snapshotPtr;
      subscribers.assign(statePtr->subscribers.begin(), statePtr->subscribers.end());
    }

    if (subscribers.empty())
    {
      return;
    }

    statePtr->dispatcherPtr->dispatch(
      [&]
      {
        for (auto const& weakSlotPtr : subscribers)
        {
          {
            auto const lock = std::scoped_lock{statePtr->mutex};

            // Another publisher (including this observer's reentry) replaced the inventory.
            if (statePtr->devicesPtr != snapshotPtr)
            {
              return;
            }
          }

          if (auto const slotPtr = weakSlotPtr.lock(); slotPtr)
          {
            statePtr->deliver(*slotPtr, *snapshotPtr);
          }
        }
      },
      std::move(stop));
  }

  void BackendDeviceRegistry::shutdown() noexcept
  {
    auto const statePtr = _statePtr;
    auto retired = std::vector<std::shared_ptr<State::Slot>>{};
    {
      auto const lock = std::scoped_lock{statePtr->mutex};
      statePtr->shutdown = true;
      statePtr->devicesPtr.reset();
      retired.swap(statePtr->subscribers);
    }

    statePtr->calls.retire();
  }
} // namespace ao::audio::backend::detail
