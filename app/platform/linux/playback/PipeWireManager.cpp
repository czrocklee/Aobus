// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/playback/PipeWireManager.h"
#include "platform/linux/playback/PipeWireBackend.h"
#include "platform/linux/playback/PipeWireMonitor.h"
#include "platform/linux/playback/detail/PipeWireShared.h"

namespace app::playback
{
  using namespace detail;

  struct PipeWireManager::Impl
  {
    std::unique_ptr<PipeWireMonitor> monitor;

    Impl()
    {
      ensurePipeWireInit();
      monitor = std::make_unique<PipeWireMonitor>();
      monitor->start();
    }

    ~Impl() { monitor.reset(); }
  };

  PipeWireManager::PipeWireManager()
    : _impl(std::make_unique<Impl>())
  {
  }
  PipeWireManager::~PipeWireManager() = default;

  void PipeWireManager::setDevicesChangedCallback(OnDevicesChangedCallback callback)
  {
    if (_impl->monitor) _impl->monitor->setDevicesChangedCallback(std::move(callback));
  }

  std::vector<app::core::backend::AudioDevice> PipeWireManager::enumerateDevices()
  {
    if (!_impl->monitor) return {};
    auto devices = _impl->monitor->enumerateSinks();
    // Add virtual "System Default" entry
    devices.insert(devices.begin(),
                   {.id = "",
                    .displayName = "System Default",
                    .description = "PipeWire",
                    .isDefault = true,
                    .backendKind = app::core::backend::BackendKind::PipeWire});
    return devices;
  }

  std::unique_ptr<app::core::backend::IAudioBackend> PipeWireManager::createBackend(
    app::core::backend::AudioDevice const& device)
  {
    return std::make_unique<PipeWireBackend>(device);
  }

  struct PipeWireSubscription final : public app::core::backend::IGraphSubscription
  {
    PipeWireMonitor* monitor;
    std::uint64_t id;
    PipeWireSubscription(PipeWireMonitor* m, std::uint64_t i)
      : monitor(m), id(i)
    {
    }
    ~PipeWireSubscription() override
    {
      if (monitor) monitor->unsubscribeGraph(id);
    }
  };

  std::unique_ptr<app::core::backend::IGraphSubscription> PipeWireManager::subscribeGraph(
    std::string_view routeAnchor,
    OnGraphChangedCallback callback)
  {
    if (!_impl->monitor) return nullptr;
    auto id = _impl->monitor->subscribeGraph(routeAnchor, std::move(callback));
    return std::make_unique<PipeWireSubscription>(_impl->monitor.get(), id);
  }
} // namespace app::playback
