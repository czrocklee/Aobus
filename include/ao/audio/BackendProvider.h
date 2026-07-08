// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/flow/Graph.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ao::audio
{
  /**
   * @brief Interface for audio backend providers (e.g., PipeWire, ALSA).
   * Provides reactive access to audio devices and system routing graphs.
   */
  class BackendProvider
  {
  public:
    /**
     * @brief Human-readable description of an audio profile.
     */
    struct ProfileDescriptor final
    {
      ProfileId id{};
      std::string name{};
      std::string description{};

      bool operator==(ProfileDescriptor const&) const = default;
    };

    /**
     * @brief Static description of an audio backend.
     */
    struct BackendDescriptor final
    {
      BackendId id{};
      std::string name{};
      std::string description{};
      std::string iconName{};
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
     * @brief Stops provider-owned asynchronous activity without destroying provider-owned state.
     *
     * After this returns, provider-owned asynchronous sources must not invoke new device or graph callbacks. Existing
     * backends created by the provider may still use provider-owned services until the provider is destroyed.
     *
     * @note Implementations must not throw.
     */
    virtual void shutdown() noexcept = 0;

    /**
     * @brief Subscribe to incremental device updates.
     * The callback is typically triggered immediately with current devices.
     * @return A subscription handle that unregisters the callback upon destruction.
     */
    virtual Subscription subscribeDevices(OnDevicesChangedCallback callback) = 0;

    /**
     * @brief Gets the current status of the provider, including supported profiles and devices.
     */
    virtual Status status() const = 0;

    /**
     * @brief Creates a backend instance for a specific device and profile.
     */
    virtual std::unique_ptr<Backend> createBackend(Device const& device, ProfileId const& profile) = 0;

    /**
     * @brief Subscribe to the system routing graph for a specific node.
     * @param routeAnchor The ID of the node to use as the root for graph discovery.
     * @return A subscription handle that unregisters the callback upon destruction.
     */
    virtual Subscription subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback) = 0;

  protected:
    BackendProvider() = default;
  };
} // namespace ao::audio
