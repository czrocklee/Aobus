// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/Subscription.h>

#include <fakeit.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace ao::audio
{
  struct Device;
}

namespace ao::audio::test
{
  class NoopRenderTarget final : public RenderTarget
  {
  public:
    RenderPcmResult renderPcm(std::span<std::byte> output) noexcept override;
    void handleUnderrun() noexcept override;
    void handlePositionAdvanced(std::uint32_t frames) noexcept override;
    void handleDrainComplete() noexcept override;
    void handleRouteReady(std::string_view routeAnchor) noexcept override;
    void handleFormatChanged(PcmFormat const& format) noexcept override;
    void handlePropertyChanged(PropertySnapshot snapshot) noexcept override;
    void handleBackendError(std::string_view message) noexcept override;
  };

  /**
   * @brief A proxy that allows using a FakeIt mock (which is a reference)
   * where a unique_ptr is required. It forwards all calls to the provided reference.
   */
  class MockBackendProxy final : public Backend
  {
  public:
    explicit MockBackendProxy(Backend& real);

    Result<OpenedPcmMode> open(SignalFormat const& f, RenderTarget& t) override;
    void start() override;
    void pause() override;
    void resume() override;
    void flush() override;
    void stop() override;
    void close() override;
    BackendId backendId() const override;
    ProfileId profileId() const override;
    Result<> setProperty(PropertyId id, PropertyValue const& value) override;

    Result<PropertyValue> property(PropertyId id) const override;
    PropertyInfo queryProperty(PropertyId id) const noexcept override;

  private:
    Backend& _real;
  };

  /**
   * @brief A proxy for BackendProvider to wrap a Mock reference into a unique_ptr.
   */
  class MockProviderProxy final : public BackendProvider
  {
  public:
    explicit MockProviderProxy(BackendProvider& real);

    void shutdown() noexcept override;

    Subscription subscribeDevices(OnDevicesChangedCallback callback) override;

    std::unique_ptr<Backend> createBackend(Device const& device, ProfileId const& profile) override;

    Status status() const override;

    Subscription subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback) override;

  private:
    BackendProvider& _real;
  };

  /**
   * @brief A Mock wrapper that uses NullBackend as a fallback (Spy mode).
   * It automatically fakes all common Backend methods to provide 'NiceMock' behavior.
   */
  template<typename T = NullBackend>
  class SpyBackend final
  {
  public:
    SpyBackend()
      : _mock{}
    {
      // Provide default 'fake' behavior for all common methods to avoid UnexpectedMethodCallException
      fakeit::When(Method(_mock, open))
        .AlwaysDo([this](SignalFormat const& sourceFormat, RenderTarget& target) -> Result<OpenedPcmMode>
                  { return _base.open(sourceFormat, target); });
      fakeit::Fake(Method(_mock, start));
      fakeit::Fake(Method(_mock, pause));
      fakeit::Fake(Method(_mock, resume));
      fakeit::Fake(Method(_mock, flush));
      fakeit::Fake(Method(_mock, stop));
      fakeit::Fake(Method(_mock, close));

      // Properties — delegate to NullBackend for stateful round-trip support
      fakeit::When(Method(_mock, setProperty))
        .AlwaysDo([this](PropertyId id, PropertyValue const& value) -> Result<>
                  { return _base.setProperty(id, value); });

      fakeit::When(Method(_mock, backendId)).AlwaysReturn(kBackendNone);
      fakeit::When(Method(_mock, profileId)).AlwaysReturn(kProfileShared);

      fakeit::When(Method(_mock, property))
        .AlwaysDo([this](PropertyId id) -> Result<PropertyValue> { return _base.property(id); });

      fakeit::When(Method(_mock, queryProperty))
        .AlwaysDo([this](PropertyId id) -> PropertyInfo { return _base.queryProperty(id); });
    }

    fakeit::Mock<Backend>& mock() { return _mock; }
    Backend& get() { return _mock.get(); }

    std::unique_ptr<Backend> makeProxy() { return std::make_unique<MockBackendProxy>(_mock.get()); }

  private:
    T _base;
    fakeit::Mock<Backend> _mock;
  };
} // namespace ao::audio::test
