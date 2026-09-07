// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/Device.h>
#include <ao/utility/ScopedRegistration.h>

#include <functional>
#include <memory>
#include <vector>

namespace ao::audio::backend::detail
{
  /** @brief Thread-safe publication point for complete backend device snapshots. */
  class BackendDeviceRegistry final
  {
  public:
    using Callback = std::function<void(std::vector<Device> const&)>;

    BackendDeviceRegistry();
    ~BackendDeviceRegistry();

    BackendDeviceRegistry(BackendDeviceRegistry const&) = delete;
    BackendDeviceRegistry& operator=(BackendDeviceRegistry const&) = delete;
    BackendDeviceRegistry(BackendDeviceRegistry&&) = delete;
    BackendDeviceRegistry& operator=(BackendDeviceRegistry&&) = delete;

    utility::ScopedRegistration subscribe(Callback callback);
    std::vector<Device> snapshot() const;
    void publish(std::vector<Device> devices);

    /** @brief Closes callback admission and clears the published snapshot. */
    void shutdown() noexcept;

  private:
    struct State;
    std::shared_ptr<State> _statePtr;
  };
} // namespace ao::audio::backend::detail
