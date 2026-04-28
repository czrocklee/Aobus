// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/backend/BackendTypes.h"

#include <functional>
#include <memory>
#include <vector>

namespace app::core::backend
{

  class IAudioBackend;

  class IGraphSubscription
  {
  public:
    virtual ~IGraphSubscription() = default;
  };

  /**
   * @brief Interface for backend-specific device monitoring and discovery.
   */
  class IBackendManager
  {
  public:
    using OnDevicesChangedCallback = std::function<void()>;
    using OnGraphChangedCallback = std::function<void(AudioGraph const&)>;

    virtual ~IBackendManager() = default;

    /**
     * @brief Sets a callback to be invoked when the list of available devices changes.
     */
    virtual void setDevicesChangedCallback(OnDevicesChangedCallback callback) = 0;

    /**
     * @brief Returns all output devices discovered by this provider.
     * Note: A single manager object may return devices with different BackendKinds
     * (e.g. PipeWire Shared and PipeWire Exclusive).
     */
    virtual std::vector<AudioDevice> enumerateDevices() = 0;

    /**
     * @brief Factory method to create a backend instance for the given device.
     * The device object passed here MUST have been previously returned by enumerateDevices().
     */
    virtual std::unique_ptr<IAudioBackend> createBackend(AudioDevice const& device) = 0;

    /**
     * @brief Subscribes to system graph updates rooted at the given anchor.
     */
    virtual std::unique_ptr<IGraphSubscription> subscribeGraph(std::string_view routeAnchor,
                                                               OnGraphChangedCallback callback) = 0;
  };

} // namespace app::core::backend
