// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "backend/PipeWireProvider.h"

#include "backend/PipeWireBackend.h"
#include "backend/PipeWireMonitor.h"
#include "backend/detail/PipeWireMonitorHooks.h"
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/Subscription.h>

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio::backend
{
  namespace
  {
    std::vector<Device> withPipeWireDefaultRoute(std::vector<Device> devices)
    {
      devices.insert(devices.begin(), {.id = DeviceId{""}, .isDefault = true, .backendId = kBackendPipeWire});
      return devices;
    }
  } // namespace

  struct PipeWireProvider::Impl final
  {
    PipeWireMonitor monitor;

    explicit Impl(std::shared_ptr<detail::PipeWireMonitorHooks> monitorHooksPtr)
      : monitor{std::move(monitorHooksPtr)}
    {
      monitor.start();
    }

    ~Impl() = default;

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
  };

  PipeWireProvider::PipeWireProvider()
    : PipeWireProvider{nullptr}
  {
  }

  PipeWireProvider::PipeWireProvider(std::shared_ptr<detail::PipeWireMonitorHooks> monitorHooksPtr)
    : _implPtr{std::make_unique<Impl>(std::move(monitorHooksPtr))}
  {
  }

  PipeWireProvider::~PipeWireProvider()
  {
    shutdown();
  }

  void PipeWireProvider::shutdown() noexcept
  {
    _implPtr->monitor.stop();
  }

  Subscription PipeWireProvider::subscribeDevices(OnDevicesChangedCallback callback)
  {
    if (!callback)
    {
      return {};
    }

    auto wrappedCallback = [callback = std::move(callback)](std::vector<Device> devices)
    { callback(withPipeWireDefaultRoute(std::move(devices))); };

    return _implPtr->monitor.subscribeDevices(std::move(wrappedCallback));
  }

  std::unique_ptr<Backend> PipeWireProvider::createBackend(Device const& device, ProfileId const& profile)
  {
    return std::make_unique<PipeWireBackend>(device, profile);
  }

  BackendProvider::Status PipeWireProvider::status() const
  {
    auto devices = _implPtr->monitor.enumerateSinks();

    if (_implPtr->monitor.isRunning())
    {
      devices = withPipeWireDefaultRoute(std::move(devices));
    }
    else
    {
      devices.clear();
    }

    return {
      .descriptor = {.id = kBackendPipeWire, .supportedProfiles = {{.id = kProfileShared}, {.id = kProfileExclusive}}},
      .devices = std::move(devices)};
  }

  Subscription PipeWireProvider::subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback)
  {
    return _implPtr->monitor.subscribeGraph(routeAnchor, std::move(callback));
  }
} // namespace ao::audio::backend
