// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "BackendTestSupport.h"

#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/Subscription.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace ao::audio::test
{
  RenderPcmResult NoopRenderTarget::renderPcm(std::span<std::byte> /*output*/) noexcept
  {
    return {.drained = true};
  }

  void NoopRenderTarget::handleUnderrun() noexcept
  {
  }

  void NoopRenderTarget::handlePositionAdvanced(std::uint32_t /*frames*/) noexcept
  {
  }

  void NoopRenderTarget::handleDrainComplete() noexcept
  {
  }

  void NoopRenderTarget::handleRouteReady(std::string_view /*routeAnchor*/) noexcept
  {
  }

  void NoopRenderTarget::handleFormatChanged(PcmFormat const& /*format*/) noexcept
  {
  }

  void NoopRenderTarget::handlePropertyChanged(PropertySnapshot /*snapshot*/) noexcept
  {
  }

  void NoopRenderTarget::handleBackendError(std::string_view /*message*/) noexcept
  {
  }

  MockBackendProxy::MockBackendProxy(Backend& real)
    : _real{real}
  {
  }

  Result<OpenedPcmMode> MockBackendProxy::open(SignalFormat const& format, RenderTarget& target)
  {
    return _real.open(format, target);
  }

  void MockBackendProxy::start()
  {
    _real.start();
  }

  void MockBackendProxy::pause()
  {
    _real.pause();
  }

  void MockBackendProxy::resume()
  {
    _real.resume();
  }

  void MockBackendProxy::flush()
  {
    _real.flush();
  }

  void MockBackendProxy::stop()
  {
    _real.stop();
  }

  void MockBackendProxy::close()
  {
    _real.close();
  }

  BackendId MockBackendProxy::backendId() const
  {
    return _real.backendId();
  }

  ProfileId MockBackendProxy::profileId() const
  {
    return _real.profileId();
  }

  Result<> MockBackendProxy::setProperty(PropertyId id, PropertyValue const& value)
  {
    return _real.setProperty(id, value);
  }

  Result<PropertyValue> MockBackendProxy::property(PropertyId id) const
  {
    return _real.property(id);
  }

  PropertyInfo MockBackendProxy::queryProperty(PropertyId id) const noexcept
  {
    return _real.queryProperty(id);
  }

  MockProviderProxy::MockProviderProxy(BackendProvider& real)
    : _real{real}
  {
  }

  void MockProviderProxy::shutdown() noexcept
  {
  }

  Subscription MockProviderProxy::subscribeDevices(OnDevicesChangedCallback callback)
  {
    return _real.subscribeDevices(callback);
  }

  std::unique_ptr<Backend> MockProviderProxy::createBackend(Device const& device, ProfileId const& profile)
  {
    return _real.createBackend(device, profile);
  }

  BackendProvider::Status MockProviderProxy::status() const
  {
    return _real.status();
  }

  Subscription MockProviderProxy::subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback)
  {
    return _real.subscribeGraph(routeAnchor, callback);
  }
} // namespace ao::audio::test
