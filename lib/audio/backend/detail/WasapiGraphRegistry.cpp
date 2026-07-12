// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Subscription.h>
#include <ao/audio/backend/detail/AudioBackendVolumeMath.h>
#include <ao/audio/backend/detail/WasapiGraphRegistry.h>
#include <ao/audio/flow/Graph.h>

#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ao::audio::backend::detail
{
  namespace
  {
    bool isUnity(float volume) noexcept
    {
      return std::abs(volume - 1.0F) < kVolumeEpsilon;
    }

    flow::Graph buildGraph(WasapiRouteState const& state)
    {
      auto graph = flow::Graph{};

      graph.nodes.push_back({.id = "wasapi-stream",
                             .type = flow::NodeType::Stream,
                             .name = "WASAPI Stream",
                             .optFormat = state.optInputFormat,
                             .objectPath = ""});

      auto sink = flow::Node{.id = "wasapi-sink",
                             .type = flow::NodeType::Sink,
                             .name = state.routeAnchor,
                             .optFormat = state.optMixFormat,
                             .objectPath = state.routeAnchor};

      sink.isMuted = state.muted;

      // Session volume is applied by the Windows audio engine in software.
      sink.maxSoftwareGain = state.volume;
      sink.minSoftwareGain = state.volume;

      if (!isUnity(state.volume))
      {
        sink.softwareVolumeNotUnity = true;
      }

      graph.nodes.push_back(std::move(sink));
      graph.connections.push_back({.sourceId = "wasapi-stream", .destinationId = "wasapi-sink", .isActive = true});

      return graph;
    }
  } // namespace

  struct WasapiGraphRegistry::Impl final
  {
    struct Subscriber
    {
      std::uint64_t id;
      std::string routeAnchor;
      Callback callback;
    };

    struct [[nodiscard]] CallbackPublicationScope final
    {
      explicit CallbackPublicationScope(Impl& owner)
        : owner{owner}
      {
        ++owner.callbackDepth;
      }

      ~CallbackPublicationScope() { --owner.callbackDepth; }

      CallbackPublicationScope(CallbackPublicationScope const&) = delete;
      CallbackPublicationScope& operator=(CallbackPublicationScope const&) = delete;
      CallbackPublicationScope(CallbackPublicationScope&&) = delete;
      CallbackPublicationScope& operator=(CallbackPublicationScope&&) = delete;

      Impl& owner;
    };

    mutable std::mutex mutex;
    mutable std::recursive_mutex callbackMutex;
    std::unordered_map<std::string, WasapiRouteState> states;
    std::vector<Subscriber> subscribers;
    std::uint64_t nextSubId = 1;
    std::size_t activeSubscriptionCount = 0;
    std::size_t callbackDepth = 0;
    bool shutdown = false;
  };

  WasapiGraphRegistry::WasapiGraphRegistry()
    : _implPtr{std::make_unique<Impl>()}
  {
  }

  WasapiGraphRegistry::~WasapiGraphRegistry()
  {
    gsl_Expects(_implPtr != nullptr);
    auto const callbackLock = std::scoped_lock{_implPtr->callbackMutex};
    gsl_Expects(_implPtr->callbackDepth == 0);
    gsl_Expects(_implPtr->activeSubscriptionCount == 0);
    auto const lock = std::scoped_lock{_implPtr->mutex};
    _implPtr->shutdown = true;
    _implPtr->states.clear();
    _implPtr->subscribers.clear();
  }

  Subscription WasapiGraphRegistry::subscribe(std::string_view routeAnchor, Callback callback)
  {
    if (!callback)
    {
      return {};
    }

    auto* const impl = _implPtr.get();
    auto const anchor = std::string{routeAnchor};
    auto initialGraph = flow::Graph{};
    std::uint64_t id = 0;
    // The callback gate covers registration, snapshot capture, and initial
    // delivery as one publication-order point. State locks are still released
    // before user code runs.
    auto const callbackLock = std::scoped_lock{impl->callbackMutex};

    {
      auto const lock = std::scoped_lock{impl->mutex};

      if (impl->shutdown)
      {
        return {};
      }

      id = impl->nextSubId++;
      impl->subscribers.push_back({.id = id, .routeAnchor = anchor, .callback = callback});

      if (auto const it = impl->states.find(anchor); it != impl->states.end())
      {
        initialGraph = buildGraph(it->second);
      }
      else
      {
        initialGraph = buildGraph({.routeAnchor = anchor});
      }
    }

    {
      auto const lock = std::scoped_lock{impl->mutex};

      if (impl->shutdown || std::ranges::find(impl->subscribers, id, &Impl::Subscriber::id) == impl->subscribers.end())
      {
        return {};
      }
    }

    try
    {
      auto publication = Impl::CallbackPublicationScope{*impl};
      callback(initialGraph);
    }
    catch (...)
    {
      auto const lock = std::scoped_lock{impl->mutex};
      auto const it = std::ranges::find(impl->subscribers, id, &Impl::Subscriber::id);

      if (it != impl->subscribers.end())
      {
        impl->subscribers.erase(it);
      }

      throw;
    }

    {
      auto const lock = std::scoped_lock{impl->mutex};

      if (impl->shutdown || std::ranges::find(impl->subscribers, id, &Impl::Subscriber::id) == impl->subscribers.end())
      {
        return {};
      }
    }

    ++impl->activeSubscriptionCount;
    return Subscription{[impl, id]
                        {
                          auto const callbackLock = std::scoped_lock{impl->callbackMutex};
                          auto const lock = std::scoped_lock{impl->mutex};
                          auto const it = std::ranges::find(impl->subscribers, id, &Impl::Subscriber::id);

                          if (it != impl->subscribers.end())
                          {
                            impl->subscribers.erase(it);
                          }

                          gsl_Expects(impl->activeSubscriptionCount != 0);
                          --impl->activeSubscriptionCount;
                        }};
  }

  void WasapiGraphRegistry::publish(WasapiRouteState state)
  {
    auto* const impl = _implPtr.get();
    auto const anchor = state.routeAnchor;
    auto const graph = buildGraph(state);
    auto pendingSubscribers = std::vector<Impl::Subscriber>{};

    {
      auto const lock = std::scoped_lock{impl->mutex};

      if (impl->shutdown)
      {
        return;
      }

      impl->states[anchor] = std::move(state);

      for (auto const& sub : impl->subscribers)
      {
        if (sub.routeAnchor == anchor)
        {
          pendingSubscribers.push_back(sub);
        }
      }
    }

    for (auto const& subscriber : pendingSubscribers)
    {
      auto const callbackLock = std::scoped_lock{impl->callbackMutex};

      {
        auto const lock = std::scoped_lock{impl->mutex};

        if (impl->shutdown)
        {
          return;
        }

        if (std::ranges::find(impl->subscribers, subscriber.id, &Impl::Subscriber::id) == impl->subscribers.end())
        {
          continue;
        }
      }

      auto publication = Impl::CallbackPublicationScope{*impl};
      subscriber.callback(graph);
    }
  }

  void WasapiGraphRegistry::clear(std::string_view routeAnchor)
  {
    auto* const impl = _implPtr.get();
    auto const anchor = std::string{routeAnchor};
    auto const emptyGraph = flow::Graph{};
    auto pendingSubscribers = std::vector<Impl::Subscriber>{};

    {
      auto const lock = std::scoped_lock{impl->mutex};

      if (impl->shutdown)
      {
        return;
      }

      impl->states.erase(anchor);

      for (auto const& sub : impl->subscribers)
      {
        if (sub.routeAnchor == anchor)
        {
          pendingSubscribers.push_back(sub);
        }
      }
    }

    for (auto const& subscriber : pendingSubscribers)
    {
      auto const callbackLock = std::scoped_lock{impl->callbackMutex};

      {
        auto const lock = std::scoped_lock{impl->mutex};

        if (impl->shutdown)
        {
          return;
        }

        if (std::ranges::find(impl->subscribers, subscriber.id, &Impl::Subscriber::id) == impl->subscribers.end())
        {
          continue;
        }
      }

      auto publication = Impl::CallbackPublicationScope{*impl};
      subscriber.callback(emptyGraph);
    }
  }

  void WasapiGraphRegistry::shutdown() noexcept
  {
    auto* const impl = _implPtr.get();
    auto const callbackLock = std::scoped_lock{impl->callbackMutex};

    {
      auto const lock = std::scoped_lock{impl->mutex};

      if (impl->shutdown)
      {
        return;
      }

      impl->shutdown = true;
      impl->states.clear();
    }

    auto const emptyGraph = flow::Graph{};

    while (true)
    {
      auto subscriber = Impl::Subscriber{};

      {
        auto const lock = std::scoped_lock{impl->mutex};

        if (impl->subscribers.empty())
        {
          return;
        }

        subscriber = std::move(impl->subscribers.front());
        impl->subscribers.erase(impl->subscribers.begin());
      }

      try
      {
        auto publication = Impl::CallbackPublicationScope{*impl};
        subscriber.callback(emptyGraph);
      }
      catch (...) // NOLINT(bugprone-empty-catch) -- shutdown is noexcept and listeners cannot retain the registry
      {
      }
    }
  }
} // namespace ao::audio::backend::detail
