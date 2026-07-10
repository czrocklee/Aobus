// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/backend/PipeWireBackend.h>
#include <ao/audio/backend/PipeWireMonitor.h>
#include <ao/audio/backend/PipeWireProvider.h>

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio::backend
{
  struct PipeWireProvider::Impl final
  {
    PipeWireMonitor monitor;

    Impl() { monitor.start(); }

    ~Impl() = default;

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
  };

  PipeWireProvider::PipeWireProvider()
    : _implPtr{std::make_unique<Impl>()}
  {
  }

  PipeWireProvider::~PipeWireProvider() = default;

  void PipeWireProvider::shutdown() noexcept
  {
    _implPtr->monitor.stop();
  }

  Subscription PipeWireProvider::subscribeDevices(OnDevicesChangedCallback callback)
  {
    // Wrap the callback to add the "System Default" virtual device
    auto wrappedCallback = [callback = std::move(callback)](std::vector<Device> devices)
    {
      devices.insert(devices.begin(),
                     {.id = DeviceId{""},
                      .displayName = "System Default",
                      .description = "PipeWire",
                      .isDefault = true,
                      .backendId = kBackendPipeWire});
      callback(devices);
    };

    return _implPtr->monitor.subscribeDevices(std::move(wrappedCallback));
  }

  std::unique_ptr<Backend> PipeWireProvider::createBackend(Device const& device, ProfileId const& profile)
  {
    return std::make_unique<PipeWireBackend>(device, profile);
  }

  BackendProvider::Status PipeWireProvider::status() const
  {
    return {.descriptor = {.id = kBackendPipeWire,
                           .name = "PipeWire",
                           .description = "Modern Linux audio server with low latency",
                           .iconName = "media-playback-start-symbolic",
                           .supportedProfiles = {{.id = kProfileShared,
                                                  .name = "Shared Mode",
                                                  .description = "System-level mixing with other applications"},
                                                 {.id = kProfileExclusive,
                                                  .name = "Exclusive Mode",
                                                  .description = "Direct access to the hardware device"}}},
            .devices = _implPtr->monitor.enumerateSinks()};
  }

  Subscription PipeWireProvider::subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback)
  {
    return _implPtr->monitor.subscribeGraph(routeAnchor, std::move(callback));
  }
} // namespace ao::audio::backend
