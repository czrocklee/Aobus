// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "BackendObserverDispatcher.h"
#include <ao/audio/flow/Graph.h>
#include <ao/utility/ScopedRegistration.h>

#include <functional>
#include <memory>
#include <stop_token>
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
   * a dispatch turn rather than handing its callbacks to the active thread.
   */
  class BackendGraphRegistry final
  {
  public:
    using Callback = std::function<void(flow::Graph const&)>;
    using DeliveryCallback = std::function<void(flow::Graph const&, bool isTerminal)>;

    /**
     * @brief One-shot observer delivery for an already committed snapshot.
     *
     * Moving or delivering empties the source. A ticket does not retain its
     * observers. Dropping it or stopping while waiting for a dispatch turn
     * skips notification without undoing the committed snapshot.
     */
    class [[nodiscard]] PendingDelivery final
    {
    public:
      PendingDelivery() = default;
      ~PendingDelivery();
      PendingDelivery(PendingDelivery const&) = delete;
      PendingDelivery& operator=(PendingDelivery const&) = delete;
      PendingDelivery(PendingDelivery&& other) noexcept;
      PendingDelivery& operator=(PendingDelivery&& other) noexcept;

      void deliver(std::stop_token stop = {}) &&;

    private:
      friend class BackendGraphRegistry;
      explicit PendingDelivery(std::function<void(std::stop_token)> callback) noexcept;
      std::function<void(std::stop_token)> _callback{};
    };

    explicit BackendGraphRegistry(
      std::shared_ptr<BackendObserverDispatcher> dispatcherPtr = std::make_shared<BackendObserverDispatcher>());
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
     * Reset closes this subscription and waits for its callbacks on other threads,
     * not unrelated graph or device observers.
     */
    utility::ScopedRegistration subscribe(std::string_view routeAnchor,
                                          Callback callback,
                                          flow::Graph initialGraph = {});

    /// As subscribe(), with explicit terminal delivery for provider lifecycle admission.
    utility::ScopedRegistration subscribeWithDelivery(std::string_view routeAnchor,
                                                      DeliveryCallback callback,
                                                      flow::Graph initialGraph = {});

    /**
     * @brief Commits a changed snapshot without waiting for or invoking observers.
     *
     * Preparation acquires only the registry state lock. An owner may prepare
     * under its snapshot lock, then release that lock before delivering the
     * returned ticket. Later preparations supersede older pending deliveries.
     */
    PendingDelivery preparePublish(std::string_view routeAnchor, flow::Graph graph);

    /// Removes the stored snapshot immediately, deferring its empty delivery.
    PendingDelivery prepareClear(std::string_view routeAnchor);

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
     * Pass false to revoke all subscriptions without a terminal notification.
     */
    void shutdown(bool deliverTerminal = true) noexcept;

  private:
    struct State;
    std::shared_ptr<State> _statePtr;
  };
} // namespace ao::audio::backend::detail
