// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/backend/PipeWireBackend.h>
#include <ao/audio/backend/PipeWireMonitor.h>
#include <ao/audio/backend/PipeWireProvider.h>
#include <ao/audio/backend/detail/PipeWireShared.h>

namespace ao::audio::backend
{
  using namespace detail;

  struct PipeWireProvider::Impl final
  {
    std::unique_ptr<PipeWireMonitor> monitor;

    Impl()
    {
      ensurePipeWireInit();
      monitor = std::make_unique<PipeWireMonitor>();
      monitor->start();
    }

    ~Impl()
    {
      if (monitor)
      {
        monitor->stop();
      }
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
  };

  PipeWireProvider::PipeWireProvider()
    : _impl{std::make_unique<Impl>()}
  {
  }

  PipeWireProvider::~PipeWireProvider() = default;

  Subscription PipeWireProvider::subscribeDevices(OnDevicesChangedCallback callback)
  {
    if (!_impl->monitor)
    {
      return Subscription{};
    }

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

    return _impl->monitor->subscribeDevices(std::move(wrappedCallback));
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
            .devices = _impl->monitor ? _impl->monitor->enumerateSinks() : std::vector<Device>{}};
  }

  Subscription PipeWireProvider::subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback)
  {
    if (!_impl->monitor)
    {
      return Subscription{};
    }

    return _impl->monitor->subscribeGraph(routeAnchor, std::move(callback));
  }
} // namespace ao::audio::backend
