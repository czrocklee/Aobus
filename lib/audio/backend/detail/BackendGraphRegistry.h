// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/flow/Graph.h>
#include <ao/utility/ScopedRegistration.h>

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
   * invocation. Equal stored graphs do not publish again. Ordinary publication,
   * clear, and initial delivery never nest callbacks: reentrant work drains
   * after the active callback returns, discarding superseded route revisions.
   * Delivery stays on the calling thread; another publishing thread waits for
   * the callback gate rather than handing its callbacks to the active thread.
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
     * @brief Subscribes to one route and publishes its initial snapshot.
     *
     * An unknown route starts with @p initialGraph, which defaults to an empty
     * graph. A stored publication takes precedence. The returned subscription
     * may safely outlive the registry. Initial delivery is synchronous except
     * when subscribe is itself called from a graph callback; in that case it
     * waits for that callback to return and can be cancelled before delivery.
     */
    utility::ScopedRegistration subscribe(std::string_view routeAnchor,
                                          Callback callback,
                                          flow::Graph initialGraph = {});

    /// Replaces a changed route snapshot and publishes it to current subscribers.
    void publish(std::string_view routeAnchor, flow::Graph graph);

    /// Removes one route snapshot and publishes an empty graph.
    void clear(std::string_view routeAnchor);

    /**
     * @brief Clears all routes, publishes final empty graphs, and closes admission.
     *
     * The call waits behind an in-flight graph callback. After it returns, no
     * new callback can begin and later subscribe/publish/clear calls are inert.
     * Retirement is separate from ordinary delivery: shutdown on a callback
     * stack synchronously delivers final empty graphs and may nest callbacks.
     * Pending ordinary deliveries are discarded, not drained after retirement.
     */
    void shutdown() noexcept;

  private:
    struct State;
    std::shared_ptr<State> _statePtr;
  };
} // namespace ao::audio::backend::detail
