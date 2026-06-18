// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/Backend.h>
#include <ao/audio/IBackend.h>
#include <ao/audio/IBackendProvider.h>
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

  std::unique_ptr<IBackend> PipeWireProvider::createBackend(Device const& device, ProfileId const& profile)
  {
    return std::make_unique<PipeWireBackend>(device, profile);
  }

  IBackendProvider::Status PipeWireProvider::status() const
  {
    return {.metadata =
              {.id = kBackendPipeWire,
               .name = "PipeWire",
               .description = "Modern Linux audio server with low latency",
               .iconName = "media-playback-start-symbolic",
               .supportedProfiles = {{kProfileShared, "Shared Mode", "System-level mixing with other applications"},
                                     {kProfileExclusive, "Exclusive Mode", "Direct access to the hardware device"}}},
            .devices = _implPtr->monitor.enumerateSinks()};
  }

  Subscription PipeWireProvider::subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback)
  {
    return _implPtr->monitor.subscribeGraph(routeAnchor, std::move(callback));
  }
} // namespace ao::audio::backend
