// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "backend/detail/BackendDeviceRegistry.h"

#include <ao/Contract.h>
#include <ao/audio/Device.h>
#include <ao/audio/Subscription.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace ao::audio::backend::detail
{
  struct BackendDeviceRegistry::State final
  {
    struct Subscriber final
    {
      std::uint64_t id = 0;
      Callback callback{};
    };

    mutable std::mutex mutex{};
    std::recursive_mutex callbackMutex{};
    std::vector<Device> devices{};
    std::vector<Subscriber> subscribers{};
    std::uint64_t nextSubscriberId = 1;
    bool shutdown = false;
  };

  namespace
  {
    bool containsSubscriber(auto const& state, std::uint64_t const id)
    {
      return std::ranges::any_of(state.subscribers, [id](auto const& subscriber) { return subscriber.id == id; });
    }

    void invokeDeviceCallback(BackendDeviceRegistry::Callback const& callback,
                              std::vector<Device> const& devices) noexcept
    {
      try
      {
        callback(devices);
      }
      catch (...)
      {
        AO_FATAL_EXCEPTION(std::current_exception(), "audio backend device observer");
      }
    }
  } // namespace

  BackendDeviceRegistry::BackendDeviceRegistry()
    : _statePtr{std::make_shared<State>()}
  {
  }

  BackendDeviceRegistry::~BackendDeviceRegistry()
  {
    shutdown();
  }

  Subscription BackendDeviceRegistry::subscribe(Callback callback)
  {
    if (!callback)
    {
      return {};
    }

    auto const statePtr = _statePtr;
    auto devices = std::vector<Device>{};
    std::uint64_t subscriberId = 0;
    auto const callbackLock = std::scoped_lock{statePtr->callbackMutex};

    {
      auto const lock = std::scoped_lock{statePtr->mutex};

      if (statePtr->shutdown)
      {
        return {};
      }

      subscriberId = statePtr->nextSubscriberId++;
      statePtr->subscribers.push_back({.id = subscriberId, .callback = std::move(callback)});
      devices = statePtr->devices;
    }

    auto initialCallback = Callback{};
    {
      auto const lock = std::scoped_lock{statePtr->mutex};
      auto const it = std::ranges::find(statePtr->subscribers, subscriberId, &State::Subscriber::id);

      if (statePtr->shutdown || it == statePtr->subscribers.end())
      {
        return {};
      }

      initialCallback = it->callback;
    }

    invokeDeviceCallback(initialCallback, devices);

    {
      auto const lock = std::scoped_lock{statePtr->mutex};

      if (statePtr->shutdown || !containsSubscriber(*statePtr, subscriberId))
      {
        return {};
      }
    }

    auto const weakStatePtr = std::weak_ptr<State>{statePtr};
    return Subscription{[weakStatePtr, subscriberId]
                        {
                          auto const statePtr = weakStatePtr.lock();

                          if (!statePtr)
                          {
                            return;
                          }

                          auto const callbackLock = std::scoped_lock{statePtr->callbackMutex};
                          auto const lock = std::scoped_lock{statePtr->mutex};
                          auto const it =
                            std::ranges::find(statePtr->subscribers, subscriberId, &State::Subscriber::id);

                          if (it != statePtr->subscribers.end())
                          {
                            statePtr->subscribers.erase(it);
                          }
                        }};
  }

  std::vector<Device> BackendDeviceRegistry::snapshot() const
  {
    auto const statePtr = _statePtr;
    auto const lock = std::scoped_lock{statePtr->mutex};
    return statePtr->devices;
  }

  void BackendDeviceRegistry::publish(std::vector<Device> devices)
  {
    auto const statePtr = _statePtr;
    auto subscribers = std::vector<State::Subscriber>{};

    {
      auto const lock = std::scoped_lock{statePtr->mutex};

      if (statePtr->shutdown)
      {
        return;
      }

      statePtr->devices = std::move(devices);
      devices = statePtr->devices;
      subscribers = statePtr->subscribers;
    }

    for (auto const& subscriber : subscribers)
    {
      auto const callbackLock = std::scoped_lock{statePtr->callbackMutex};
      {
        auto const lock = std::scoped_lock{statePtr->mutex};

        if (statePtr->shutdown)
        {
          return;
        }

        if (!containsSubscriber(*statePtr, subscriber.id))
        {
          continue;
        }
      }
      invokeDeviceCallback(subscriber.callback, devices);
    }
  }

  void BackendDeviceRegistry::shutdown() noexcept
  {
    auto const statePtr = _statePtr;
    auto const callbackLock = std::scoped_lock{statePtr->callbackMutex};
    auto const lock = std::scoped_lock{statePtr->mutex};
    statePtr->shutdown = true;
    statePtr->devices.clear();
    statePtr->subscribers.clear();
  }
} // namespace ao::audio::backend::detail
