// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/playback/PipeWireProvider.h"
#include "platform/linux/playback/PipeWireBackend.h"
#include "platform/linux/playback/PipeWireMonitor.h"
#include "platform/linux/playback/detail/PipeWireShared.h"

namespace app::playback
{
  using namespace detail;

  struct PipeWireProvider::Impl
  {
    std::unique_ptr<PipeWireMonitor> monitor;

    Impl()
    {
      ensurePipeWireInit();
      monitor = std::make_unique<PipeWireMonitor>();
      monitor->start();
    }

    ~Impl() { monitor.reset(); }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
  };

  PipeWireProvider::PipeWireProvider()
    : _impl(std::make_unique<Impl>())
  {
  }
  PipeWireProvider::~PipeWireProvider() = default;

  rs::audio::Subscription PipeWireProvider::subscribeDevices(OnDevicesChangedCallback callback)
  {
    if (!_impl->monitor)
    {
      return rs::audio::Subscription{};
    }

    // Wrap the callback to add the "System Default" virtual device
    auto wrappedCallback = [callback = std::move(callback)](std::vector<rs::audio::Device> devices)
    {
      devices.insert(devices.begin(),
                     {.id = "",
                      .displayName = "System Default",
                      .description = "PipeWire",
                      .isDefault = true,
                      .backendKind = rs::audio::BackendKind::PipeWire});
      callback(devices);
    };

    return _impl->monitor->subscribeDevices(std::move(wrappedCallback));
  }

  std::unique_ptr<rs::audio::IBackend> PipeWireProvider::createBackend(rs::audio::Device const& device)
  {
    return std::make_unique<PipeWireBackend>(device);
  }

  rs::audio::Subscription PipeWireProvider::subscribeGraph(std::string_view routeAnchor,
                                                           OnGraphChangedCallback callback)
  {
    if (!_impl->monitor)
    {
      return rs::audio::Subscription{};
    }
    return _impl->monitor->subscribeGraph(routeAnchor, std::move(callback));
  }
} // namespace app::playback
