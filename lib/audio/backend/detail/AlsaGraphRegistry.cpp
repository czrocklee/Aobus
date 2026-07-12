// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Subscription.h>
#include <ao/audio/backend/detail/AlsaGraphRegistry.h>
#include <ao/audio/backend/detail/AudioBackendVolumeMath.h>
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

    flow::Graph buildGraph(AlsaRouteState const& state)
    {
      auto graph = flow::Graph{};

      graph.nodes.push_back({.id = "alsa-stream",
                             .type = flow::NodeType::Stream,
                             .name = "ALSA Stream",
                             .optFormat = state.optFormat,
                             .objectPath = ""});

      auto sink = flow::Node{.id = "alsa-sink",
                             .type = flow::NodeType::Sink,
                             .name = state.routeAnchor,
                             .optFormat = state.optFormat,
                             .objectPath = state.routeAnchor};

      sink.isMuted = state.muted;

      if (state.volumeMode == AlsaVolumeControlMode::SoftwareGain)
      {
        sink.maxSoftwareGain = state.volume;
        sink.minSoftwareGain = state.volume;
      }

      if (!isUnity(state.volume))
      {
        if (state.volumeMode == AlsaVolumeControlMode::HardwareMixer)
        {
          sink.hardwareVolumeNotUnity = true;
        }
        else if (state.volumeMode == AlsaVolumeControlMode::SoftwareGain)
        {
          sink.softwareVolumeNotUnity = true;
        }
        else
        {
          sink.unclassifiedVolumeNotUnity = true;
        }
      }

      graph.nodes.push_back(std::move(sink));
      graph.connections.push_back({.sourceId = "alsa-stream", .destinationId = "alsa-sink", .isActive = true});

      return graph;
    }
  } // namespace

  struct AlsaGraphRegistry::Impl final
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
    std::unordered_map<std::string, AlsaRouteState> states;
    std::vector<Subscriber> subscribers;
    std::uint64_t nextSubId = 1;
    std::size_t activeSubscriptionCount = 0;
    std::size_t callbackDepth = 0;
    bool shutdown = false;
  };

  AlsaGraphRegistry::AlsaGraphRegistry()
    : _implPtr{std::make_unique<Impl>()}
  {
  }

  AlsaGraphRegistry::~AlsaGraphRegistry()
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

  Subscription AlsaGraphRegistry::subscribe(std::string_view routeAnchor, Callback callback)
  {
    if (!callback)
    {
      return {};
    }

    auto* const impl = _implPtr.get();
    auto const anchor = std::string{routeAnchor};
    auto initialGraph = flow::Graph{};
    std::uint64_t id = 0;
    // Linearize registration, snapshot capture, and initial delivery with
    // publications while keeping the state mutex out of user callbacks.
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
                          auto const callbackLockInside = std::scoped_lock{impl->callbackMutex};
                          auto const lockInside = std::scoped_lock{impl->mutex};
                          auto const it = std::ranges::find(impl->subscribers, id, &Impl::Subscriber::id);

                          if (it != impl->subscribers.end())
                          {
                            impl->subscribers.erase(it);
                          }

                          gsl_Expects(impl->activeSubscriptionCount != 0);
                          --impl->activeSubscriptionCount;
                        }};
  }

  void AlsaGraphRegistry::publish(AlsaRouteState state)
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

  void AlsaGraphRegistry::clear(std::string_view routeAnchor)
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
} // namespace ao::audio::backend::detail
