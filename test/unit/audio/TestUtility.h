// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "fakeit.hpp"
#include <ao/audio/IBackend.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/NullBackend.h>
#include <ao/utility/IMainThreadDispatcher.h>
#include <memory>

namespace ao::audio::test
{
  /**
   * @brief A simple dispatcher that executes callbacks immediately.
   */
  class MockDispatcher : public ao::IMainThreadDispatcher
  {
  public:
    void dispatch(std::function<void()> callback) override { callback(); }
  };

  /**
   * @brief A proxy that allows using a FakeIt mock (which is a reference)
   * where a unique_ptr is required. It forwards all calls to the provided reference.
   */
  class MockBackendProxy final : public IBackend
  {
  public:
    explicit MockBackendProxy(IBackend& real)
      : _real{real}
    {
    }

    ao::Result<> open(Format const& f, RenderCallbacks c) override { return _real.open(f, c); }
    void start() override { _real.start(); }
    void pause() override { _real.pause(); }
    void resume() override { _real.resume(); }
    void flush() override { _real.flush(); }
    void stop() override { _real.stop(); }
    void close() override { _real.close(); }
    BackendId backendId() const noexcept override { return _real.backendId(); }
    ProfileId profileId() const noexcept override { return _real.profileId(); }
    ao::Result<> setProperty(PropertyId id, PropertyValue const& value) override
    {
      return _real.setProperty(id, value);
    }
    ao::Result<PropertyValue> getProperty(PropertyId id) const override { return _real.getProperty(id); }
    PropertyInfo queryProperty(PropertyId id) const noexcept override { return _real.queryProperty(id); }

  private:
    IBackend& _real;
  };

  /**
   * @brief A proxy for IBackendProvider to wrap a Mock reference into a unique_ptr.
   */
  class MockProviderProxy final : public IBackendProvider
  {
  public:
    explicit MockProviderProxy(IBackendProvider& real)
      : _real{real}
    {
    }

    Subscription subscribeDevices(OnDevicesChangedCallback callback) override
    {
      return _real.subscribeDevices(callback);
    }

    std::unique_ptr<IBackend> createBackend(Device const& device, ProfileId const& profile) override
    {
      return _real.createBackend(device, profile);
    }

    Status status() const override { return _real.status(); }

    Subscription subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback) override
    {
      return _real.subscribeGraph(routeAnchor, callback);
    }

  private:
    IBackendProvider& _real;
  };

  /**
   * @brief A Mock wrapper that uses NullBackend as a fallback (Spy mode).
   * It automatically fakes all common IBackend methods to provide 'NiceMock' behavior.
   */
  template<typename T = NullBackend>
  class SpyBackend
  {
  public:
    SpyBackend()
      : _mock{_base}
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
        .AlwaysDo([this](PropertyId id, PropertyValue const& value) -> ao::Result<>
                  { return _base.setProperty(id, value); });

      fakeit::When(Method(_mock, backendId)).AlwaysReturn(kBackendNone);
      fakeit::When(Method(_mock, profileId)).AlwaysReturn(kProfileShared);

      fakeit::When(Method(_mock, getProperty))
        .AlwaysDo([this](PropertyId id) -> ao::Result<PropertyValue> { return _base.getProperty(id); });

      fakeit::When(Method(_mock, queryProperty))
        .AlwaysDo([this](PropertyId id) -> PropertyInfo { return _base.queryProperty(id); });
    }

    fakeit::Mock<IBackend>& mock() { return _mock; }
    IBackend& get() { return _mock.get(); }

    std::unique_ptr<IBackend> make_proxy() { return std::make_unique<MockBackendProxy>(_mock.get()); }

  private:
    T _base;
    fakeit::Mock<IBackend> _mock;
  };
} // namespace ao::audio::test
