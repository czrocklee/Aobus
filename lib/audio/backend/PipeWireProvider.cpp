// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/backend/PipeWireBackend.h>
#include <ao/audio/backend/PipeWireMonitor.h>
#include <ao/audio/backend/PipeWireProvider.h>
#include <ao/audio/backend/detail/PipeWireShared.h>

namespace ao::audio::backend
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
    : _impl{std::make_unique<Impl>()}
  {
  }
  PipeWireProvider::~PipeWireProvider() = default;

  ao::audio::Subscription PipeWireProvider::subscribeDevices(OnDevicesChangedCallback callback)
  {
    if (!_impl->monitor)
    {
      return ao::audio::Subscription{};
    }

    // Wrap the callback to add the "System Default" virtual device
    auto wrappedCallback = [callback = std::move(callback)](std::vector<ao::audio::Device> devices)
    {
      devices.insert(devices.begin(),
                     {.id = "",
                      .displayName = "System Default",
                      .description = "PipeWire",
                      .isDefault = true,
                      .backendKind = ao::audio::BackendKind::PipeWire});
      callback(devices);
    };

    return _impl->monitor->subscribeDevices(std::move(wrappedCallback));
  }

  std::unique_ptr<ao::audio::IBackend> PipeWireProvider::createBackend(ao::audio::Device const& device)
  {
    return std::make_unique<PipeWireBackend>(device);
  }

  ao::audio::Subscription PipeWireProvider::subscribeGraph(std::string_view routeAnchor,
                                                           OnGraphChangedCallback callback)
  {
    if (!_impl->monitor)
    {
      return ao::audio::Subscription{};
    }
    return _impl->monitor->subscribeGraph(routeAnchor, std::move(callback));
  }
} // namespace ao::audio::backend
