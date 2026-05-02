// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/audio/IBackend.h>

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace ao::audio
{
  /**
   * @brief A move-only handle that unsubscribes a listener when destroyed.
   */
  class [[nodiscard]] Subscription final
  {
  public:
    Subscription() = default;
    explicit Subscription(std::move_only_function<void()> unsub)
      : _unsub(std::move(unsub))
    {
    }

    ~Subscription()
    {
      if (_unsub)
      {
        _unsub();
      }
    }

    Subscription(Subscription const&) = delete;
    Subscription& operator=(Subscription const&) = delete;

    Subscription(Subscription&&) noexcept = default;
    Subscription& operator=(Subscription&&) noexcept = default;

    /**
     * @brief Manually trigger unsubscription.
     */
    void reset() { _unsub = nullptr; }

    /**
     * @brief Returns true if the subscription is active.
     */
    explicit operator bool() const noexcept { return static_cast<bool>(_unsub); }

  private:
    std::move_only_function<void()> _unsub;
  };

  /**
   * @brief Interface for audio backend providers (e.g., PipeWire, ALSA).
   * Provides reactive access to audio devices and system routing graphs.
   */
  class IBackendProvider
  {
  public:
    using OnDevicesChangedCallback = std::function<void(std::vector<Device> const&)>;
    using OnGraphChangedCallback = std::function<void(flow::Graph const&)>;

    virtual ~IBackendProvider() = default;

    /**
     * @brief Subscribe to incremental device updates.
     * The callback is typically triggered immediately with current devices.
     * @return A subscription handle that unregisters the callback upon destruction.
     */
    virtual Subscription subscribeDevices(OnDevicesChangedCallback callback) = 0;

    /**
     * @brief Creates a backend instance for a specific device.
     */
    virtual std::unique_ptr<IBackend> createBackend(Device const& device) = 0;

    /**
     * @brief Subscribe to the system routing graph for a specific node.
     * @param routeAnchor The ID of the node to use as the root for graph discovery.
     * @return A subscription handle that unregisters the callback upon destruction.
     */
    virtual Subscription subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback) = 0;
  };
} // namespace ao::audio
