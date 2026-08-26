// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/Subscription.h>
#include <ao/audio/flow/Graph.h>

#include <functional>
#include <memory>
#include <string_view>

namespace ao::audio::backend::detail
{
  /**
   * @brief Thread-safe registry for backend-owned route graph snapshots.
   *
   * Backends publish complete graphs keyed by their stable route anchor. A
   * provider serves subscriptions from the same registry without exposing
   * backend-specific state. Callback delivery is serialized, happens without
   * the registry state lock, and rechecks cancellation immediately before
   * invocation.
   */
  class BackendGraphRegistry final
  {
  public:
    using Callback = std::function<void(flow::Graph const&)>;

    BackendGraphRegistry();
    ~BackendGraphRegistry();

    BackendGraphRegistry(BackendGraphRegistry const&) = delete;
    BackendGraphRegistry& operator=(BackendGraphRegistry const&) = delete;
    BackendGraphRegistry(BackendGraphRegistry&&) = delete;
    BackendGraphRegistry& operator=(BackendGraphRegistry&&) = delete;

    /**
     * @brief Subscribes to one route and immediately publishes its snapshot.
     *
     * An unknown route starts with @p initialGraph, which defaults to an empty
     * graph. A stored publication takes precedence. The returned subscription
     * may safely outlive the registry.
     */
    Subscription subscribe(std::string_view routeAnchor, Callback callback, flow::Graph initialGraph = {});

    /// Replaces one route snapshot and publishes it to current subscribers.
    void publish(std::string_view routeAnchor, flow::Graph graph);

    /// Removes one route snapshot and publishes an empty graph.
    void clear(std::string_view routeAnchor);

    /**
     * @brief Clears all routes, publishes final empty graphs, and closes admission.
     *
     * The call waits behind an in-flight graph callback. After it returns, no
     * new callback can begin and later subscribe/publish/clear calls are inert.
     */
    void shutdown() noexcept;

  private:
    struct State;
    std::shared_ptr<State> _statePtr;
  };
} // namespace ao::audio::backend::detail
