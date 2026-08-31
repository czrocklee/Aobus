// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/flow/Graph.h>

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace ao::audio
{
  struct Device;

  /**
   * @brief Interface for audio backend providers (e.g., PipeWire, ALSA).
   * Provides reactive access to audio devices and system routing graphs.
   */
  class BackendProvider
  {
  public:
    struct ProfileDescriptor final
    {
      ProfileId id{};

      bool operator==(ProfileDescriptor const&) const = default;
    };

    struct BackendDescriptor final
    {
      BackendId id{};
      std::vector<ProfileDescriptor> supportedProfiles{};

      bool operator==(BackendDescriptor const&) const = default;
    };

    struct Status final
    {
      BackendDescriptor descriptor{};
      std::vector<Device> devices{};

      bool operator==(Status const&) const = default;
    };

    using OnDevicesChangedCallback = std::function<void(std::vector<Device> const&)>;
    using OnGraphChangedCallback = std::function<void(flow::Graph const&)>;

    virtual ~BackendProvider() = default;

    BackendProvider(BackendProvider const&) = delete;
    BackendProvider& operator=(BackendProvider const&) = delete;
    BackendProvider(BackendProvider&&) = delete;
    BackendProvider& operator=(BackendProvider&&) = delete;

    /**
     * @brief Closes callback admission and stops provider-owned asynchronous activity.
     *
     * A call outside an admitted provider callback returns only after workers have
     * stopped and every previously admitted callback has completed. A callback
     * may initiate shutdown without waiting on itself; independently retained
     * provider state completes quiescence after that callback returns. Repeated
     * and concurrent calls share the same completion boundary. Registry
     * retirement may synchronously deliver an already-accepted terminal snapshot
     * to an external initiator; callback-origin retirement does not recursively
     * admit another callback for that provider.
     *
     * After admission closes, subscriptions are inert and no new device or graph
     * callback begins. Subscription handles and backends already returned by the
     * provider remain safe to destroy after the provider facade; they retain only
     * the narrow state needed for unregistration or backend publication, and a
     * retired publication route stays inert.
     *
     * @note Implementations must not throw.
     */
    virtual void shutdown() noexcept = 0;

    /**
     * @brief Subscribes to incremental device updates.
     *
     * An accepted callback is typically invoked immediately with current
     * devices and may destroy the provider. Concurrent accepted subscriptions
     * are independent. Once shutdown closes admission, this returns an inert
     * handle without invoking the callback. The returned handle may outlive the
     * provider and remains safe to reset concurrently with publication.
     */
    virtual Subscription subscribeDevices(OnDevicesChangedCallback callback) = 0;

    /**
     * @brief Gets the current status of the provider, including supported profiles and devices.
     *
     * While admission is open, the device inventory uses the same provider-level
     * representation as a subscribeDevices() snapshot, including virtual routes.
     * After shutdown closes admission, the device inventory is empty.
     */
    virtual Status status() const = 0;

    /**
     * @brief Creates a backend instance for a specific device and profile.
     *
     * A returned backend may outlive this provider facade. Provider retirement
     * must make any retained graph-publication route inert before releasing the
     * state that route needs.
     */
    virtual std::unique_ptr<Backend> createBackend(Device const& device, ProfileId const& profile) = 0;

    /**
     * @brief Subscribes to the system routing graph for a specific node.
     * @param routeAnchor The ID of the node to use as the root for graph discovery.
     *
     * This has the same callback-destroys-provider, concurrent reset, inert
     * post-shutdown, and provider-independent handle lifetime contract as
     * subscribeDevices().
     */
    virtual Subscription subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback) = 0;

  protected:
    BackendProvider() = default;
  };

  /**
   * @brief Creates the native audio backend providers in platform preference order.
   *
   * Linux prefers PipeWire and keeps ALSA as the fallback. Windows provides
   * WASAPI, and macOS provides Core Audio.
   */
  std::vector<std::unique_ptr<BackendProvider>> createPlatformBackendProviders();
} // namespace ao::audio
