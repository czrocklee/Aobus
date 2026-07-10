// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/Property.h>
#include <ao/audio/Subscription.h>

#include <fakeit.hpp>

#include <memory>
#include <string_view>

namespace ao::audio
{
  struct Format;
  class RenderTarget;
}

namespace ao::audio::test
{
  /**
   * @brief A proxy that allows using a FakeIt mock (which is a reference)
   * where a unique_ptr is required. It forwards all calls to the provided reference.
   */
  class MockBackendProxy final : public Backend
  {
  public:
    explicit MockBackendProxy(Backend& real)
      : _real{real}
    {
    }

    Result<> open(Format const& f, RenderTarget* t) override { return _real.open(f, t); }
    void start() override { _real.start(); }
    void pause() override { _real.pause(); }
    void resume() override { _real.resume(); }
    void flush() override { _real.flush(); }
    void stop() override { _real.stop(); }
    void close() override { _real.close(); }
    BackendId backendId() const override { return _real.backendId(); }
    ProfileId profileId() const override { return _real.profileId(); }
    Result<> setProperty(PropertyId id, PropertyValue const& value) override { return _real.setProperty(id, value); }

    Result<PropertyValue> property(PropertyId id) const override { return _real.property(id); }
    PropertyInfo queryProperty(PropertyId id) const noexcept override { return _real.queryProperty(id); }

  private:
    Backend& _real;
  };

  /**
   * @brief A proxy for BackendProvider to wrap a Mock reference into a unique_ptr.
   */
  class MockProviderProxy final : public BackendProvider
  {
  public:
    explicit MockProviderProxy(BackendProvider& real)
      : _real{real}
    {
    }

    void shutdown() noexcept override {}

    Subscription subscribeDevices(OnDevicesChangedCallback callback) override
    {
      return _real.subscribeDevices(callback);
    }

    std::unique_ptr<Backend> createBackend(Device const& device, ProfileId const& profile) override
    {
      return _real.createBackend(device, profile);
    }

    Status status() const override { return _real.status(); }

    Subscription subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback) override
    {
      return _real.subscribeGraph(routeAnchor, callback);
    }

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
      fakeit::Fake(Method(_mock, open));
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
