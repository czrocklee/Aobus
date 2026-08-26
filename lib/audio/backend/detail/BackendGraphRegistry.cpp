// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "backend/detail/BackendGraphRegistry.h"

#include <ao/Contract.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/flow/Graph.h>

#include <algorithm>
#include <cstdint>
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

    std::mutex mutex{};
    std::recursive_mutex callbackMutex{};
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

    void publishToSubscribers(auto const& statePtr,
                              auto const& subscribers,
                              std::string const& routeAnchor,
                              std::uint64_t const revision,
                              flow::Graph const& graph)
    {
      for (auto const& subscriber : subscribers)
      {
        auto const callbackLock = std::scoped_lock{statePtr->callbackMutex};

        {
          auto const lock = std::scoped_lock{statePtr->mutex};

          if (statePtr->shutdown)
          {
            return;
          }

          auto const revisionIt = statePtr->revisions.find(routeAnchor);

          if (revisionIt == statePtr->revisions.end() || revisionIt->second != revision)
          {
            return;
          }

          if (!containsSubscriber(*statePtr, subscriber.id))
          {
            continue;
          }
        }

        invokeGraphCallback(subscriber.callback, graph, "audio backend graph observer");
      }
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

  Subscription BackendGraphRegistry::subscribe(std::string_view const routeAnchor,
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

    invokeGraphCallback(initialCallback, initialGraph, "audio backend initial graph observer");

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

  void BackendGraphRegistry::publish(std::string_view const routeAnchor, flow::Graph graph)
  {
    auto const statePtr = _statePtr;
    auto const anchor = std::string{routeAnchor};
    auto pendingSubscribers = std::vector<State::Subscriber>{};
    std::uint64_t revision = 0;

    {
      auto const lock = std::scoped_lock{statePtr->mutex};

      if (statePtr->shutdown)
      {
        return;
      }

      statePtr->graphs[anchor] = graph;
      revision = statePtr->nextRevision++;
      statePtr->revisions[anchor] = revision;

      for (auto const& subscriber : statePtr->subscribers)
      {
        if (subscriber.routeAnchor == anchor)
        {
          pendingSubscribers.push_back(subscriber);
        }
      }
    }

    publishToSubscribers(statePtr, pendingSubscribers, anchor, revision, graph);
  }

  void BackendGraphRegistry::clear(std::string_view const routeAnchor)
  {
    auto const statePtr = _statePtr;
    auto const anchor = std::string{routeAnchor};
    auto pendingSubscribers = std::vector<State::Subscriber>{};
    std::uint64_t revision = 0;

    {
      auto const lock = std::scoped_lock{statePtr->mutex};

      if (statePtr->shutdown)
      {
        return;
      }

      statePtr->graphs.erase(anchor);
      revision = statePtr->nextRevision++;
      statePtr->revisions[anchor] = revision;

      for (auto const& subscriber : statePtr->subscribers)
      {
        if (subscriber.routeAnchor == anchor)
        {
          pendingSubscribers.push_back(subscriber);
        }
      }
    }

    publishToSubscribers(statePtr, pendingSubscribers, anchor, revision, {});
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
