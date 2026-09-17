// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "backend/detail/BackendGraphRegistry.h"

#include <ao/Contract.h>
#include <ao/audio/flow/Graph.h>
#include <ao/utility/ScopedRegistration.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ao::audio::backend::detail
{
  struct BackendGraphRegistry::State final
  {
    struct Subscriber final
    {
      std::uint64_t id = 0;
      std::string routeAnchor{};
      Callback callback{};
    };

    struct Delivery final
    {
      std::string routeAnchor{};
      // Initial subscription snapshots precede later publications and have no route revision.
      std::uint64_t revision = 0;
      flow::Graph graph{};
      std::vector<Subscriber> subscribers{};
      char const* context = "audio backend graph observer";
    };

    std::mutex mutex{};
    std::recursive_mutex callbackMutex{};
    // Only the callback-gate owner may enqueue or drain reentrant delivery.
    // Other publishing threads retain their own delivery while waiting for the gate.
    std::deque<Delivery> pendingDeliveries{};
    bool delivering = false;
    std::unordered_map<std::string, flow::Graph> graphs{};
    std::unordered_map<std::string, std::uint64_t> revisions{};
    std::vector<Subscriber> subscribers{};
    std::uint64_t nextSubscriberId = 1;
    std::uint64_t nextRevision = 1;
    bool shutdown = false;
  };

  namespace
  {
    bool containsSubscriber(auto const& state, std::uint64_t const id)
    {
      return std::ranges::any_of(state.subscribers, [id](auto const& subscriber) { return subscriber.id == id; });
    }

    void invokeGraphCallback(BackendGraphRegistry::Callback const& callback,
                             flow::Graph const& graph,
                             char const* const context) noexcept
    {
      try
      {
        callback(graph);
      }
      catch (...)
      {
        AO_FATAL_EXCEPTION(std::current_exception(), context);
      }
    }

    void publishToSubscribers(auto const& statePtr, auto const& delivery)
    {
      for (auto const& subscriber : delivery.subscribers)
      {
        {
          auto const lock = std::scoped_lock{statePtr->mutex};

          if (statePtr->shutdown)
          {
            return;
          }

          auto const revisionIt = statePtr->revisions.find(delivery.routeAnchor);

          if (delivery.revision != 0 &&
              (revisionIt == statePtr->revisions.end() || revisionIt->second != delivery.revision))
          {
            return;
          }

          if (!containsSubscriber(*statePtr, subscriber.id))
          {
            continue;
          }
        }

        invokeGraphCallback(subscriber.callback, delivery.graph, delivery.context);
      }
    }

    void deliverGraph(auto const& statePtr, auto delivery)
    {
      auto const callbackLock = std::scoped_lock{statePtr->callbackMutex};
      statePtr->pendingDeliveries.push_back(std::move(delivery));

      if (statePtr->delivering)
      {
        return;
      }

      statePtr->delivering = true;

      while (!statePtr->pendingDeliveries.empty())
      {
        auto nextDelivery = std::move(statePtr->pendingDeliveries.front());
        statePtr->pendingDeliveries.pop_front();
        // Revision checks discard superseded reentrant snapshots before their callbacks run.
        publishToSubscribers(statePtr, nextDelivery);
      }

      statePtr->delivering = false;
    }
  } // namespace

  BackendGraphRegistry::BackendGraphRegistry()
    : _statePtr{std::make_shared<State>()}
  {
  }

  BackendGraphRegistry::~BackendGraphRegistry()
  {
    shutdown();
  }

  utility::ScopedRegistration BackendGraphRegistry::subscribe(std::string_view const routeAnchor,
                                                              Callback callback,
                                                              flow::Graph initialGraph)
  {
    if (!callback)
    {
      return {};
    }

    auto const statePtr = _statePtr;
    auto const anchor = std::string{routeAnchor};
    std::uint64_t subscriberId = 0;
    auto delivery = State::Delivery{.routeAnchor = anchor, .context = "audio backend initial graph observer"};
    auto const callbackLock = std::scoped_lock{statePtr->callbackMutex};

    {
      auto const lock = std::scoped_lock{statePtr->mutex};

      if (statePtr->shutdown)
      {
        return {};
      }

      subscriberId = statePtr->nextSubscriberId++;
      statePtr->subscribers.push_back({.id = subscriberId, .routeAnchor = anchor, .callback = std::move(callback)});

      if (auto const it = statePtr->graphs.find(anchor); it != statePtr->graphs.end())
      {
        initialGraph = it->second;
      }

      delivery.graph = std::move(initialGraph);
      delivery.subscribers.push_back(statePtr->subscribers.back());
    }

    deliverGraph(statePtr, std::move(delivery));

    {
      auto const lock = std::scoped_lock{statePtr->mutex};

      if (statePtr->shutdown || !containsSubscriber(*statePtr, subscriberId))
      {
        return {};
      }
    }

    auto const weakStatePtr = std::weak_ptr<State>{statePtr};
    return utility::ScopedRegistration{[weakStatePtr, subscriberId]
                                       {
                                         auto const statePtr = weakStatePtr.lock();

                                         if (!statePtr)
                                         {
                                           return;
                                         }

                                         auto const callbackLock = std::scoped_lock{statePtr->callbackMutex};
                                         auto const lock = std::scoped_lock{statePtr->mutex};
                                         auto const it = std::ranges::find(
                                           statePtr->subscribers, subscriberId, &State::Subscriber::id);

                                         if (it != statePtr->subscribers.end())
                                         {
                                           statePtr->subscribers.erase(it);
                                         }
                                       }};
  }

  void BackendGraphRegistry::publish(std::string_view const routeAnchor, flow::Graph graph)
  {
    auto const statePtr = _statePtr;
    auto const anchor = std::string{routeAnchor};
    auto delivery = State::Delivery{.routeAnchor = anchor, .graph = std::move(graph)};

    {
      auto const lock = std::scoped_lock{statePtr->mutex};

      if (statePtr->shutdown)
      {
        return;
      }

      if (auto const it = statePtr->graphs.find(anchor); it != statePtr->graphs.end() && it->second == delivery.graph)
      {
        return;
      }

      statePtr->graphs[anchor] = delivery.graph;
      delivery.revision = statePtr->nextRevision++;
      statePtr->revisions[anchor] = delivery.revision;

      for (auto const& subscriber : statePtr->subscribers)
      {
        if (subscriber.routeAnchor == anchor)
        {
          delivery.subscribers.push_back(subscriber);
        }
      }
    }

    deliverGraph(statePtr, std::move(delivery));
  }

  void BackendGraphRegistry::clear(std::string_view const routeAnchor)
  {
    auto const statePtr = _statePtr;
    auto const anchor = std::string{routeAnchor};
    auto delivery = State::Delivery{.routeAnchor = anchor};

    {
      auto const lock = std::scoped_lock{statePtr->mutex};

      if (statePtr->shutdown)
      {
        return;
      }

      statePtr->graphs.erase(anchor);
      delivery.revision = statePtr->nextRevision++;
      statePtr->revisions[anchor] = delivery.revision;

      for (auto const& subscriber : statePtr->subscribers)
      {
        if (subscriber.routeAnchor == anchor)
        {
          delivery.subscribers.push_back(subscriber);
        }
      }
    }

    deliverGraph(statePtr, std::move(delivery));
  }

  void BackendGraphRegistry::shutdown() noexcept
  {
    auto const statePtr = _statePtr;
    auto const callbackLock = std::scoped_lock{statePtr->callbackMutex};

    {
      auto const lock = std::scoped_lock{statePtr->mutex};

      if (statePtr->shutdown)
      {
        return;
      }

      statePtr->shutdown = true;
      statePtr->graphs.clear();
      statePtr->revisions.clear();
    }

    // Retirement is synchronous even on a callback stack. Do not defer final
    // empties: the caller may destroy its observer state as soon as shutdown returns.
    statePtr->pendingDeliveries.clear();
    auto const emptyGraph = flow::Graph{};

    while (true)
    {
      auto subscriber = State::Subscriber{};

      {
        auto const lock = std::scoped_lock{statePtr->mutex};

        if (statePtr->subscribers.empty())
        {
          return;
        }

        subscriber = std::move(statePtr->subscribers.front());
        statePtr->subscribers.erase(statePtr->subscribers.begin());
      }

      invokeGraphCallback(subscriber.callback, emptyGraph, "audio backend graph shutdown observer");
    }
  }
} // namespace ao::audio::backend::detail
