// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ao/audio/IBackend.h>
#include <string>
#include <vector>

namespace ao::audio
{
  class CapturingBackend final : public IBackend
  {
  public:
    struct Event
    {
      std::string name;
      Format format;
    };

    ao::Result<> open(Format const& format, RenderCallbacks callbacks) override
    {
      _events.push_back({"open", format});
      _callbacks = callbacks;
      _format = format;
      return _openResult;
    }

    void start() override { _events.push_back({"start", {}}); }
    void pause() override { _events.push_back({"pause", {}}); }
    void resume() override { _events.push_back({"resume", {}}); }
    void flush() override { _events.push_back({"flush", {}}); }
    void stop() override { _events.push_back({"stop", {}}); }
    void close() override { _events.push_back({"close", {}}); }

    BackendId backendId() const noexcept override { return BackendId{"capturing"}; }
    ProfileId profileId() const noexcept override { return ProfileId{"test"}; }

    Result<> setProperty(PropertyId id, PropertyValue const& value) override
    {
      if (id == PropertyId::Volume)
      {
        _volume = std::get<float>(value);
        return {};
      }

      if (id == PropertyId::Muted)
      {
        _muted = std::get<bool>(value);
        return {};
      }
      return std::unexpected(ao::Error{.code = ao::Error::Code::NotSupported});
    }

    Result<PropertyValue> getProperty(PropertyId id) const override
    {
      if (id == PropertyId::Volume)
      {
        return _volume;
      }

      if (id == PropertyId::Muted)
      {
        return _muted;
      }
      return std::unexpected(ao::Error{.code = ao::Error::Code::NotSupported});
    }

    PropertyInfo queryProperty(PropertyId id) const noexcept override
    {
      if (id == PropertyId::Volume || id == PropertyId::Muted)
      {
        return {.canRead = true, .canWrite = true, .isAvailable = true, .emitsChangeNotifications = false};
      }
      return {};
    }

    // Helpers for tests
    void setOpenResult(ao::Result<> res) { _openResult = res; }
    std::vector<Event> const& events() const { return _events; }
    void clearEvents() { _events.clear(); }
    RenderCallbacks const& callbacks() const { return _callbacks; }
    Format currentFormat() const { return _format; }

    // Trigger callbacks
    void fireRouteReady(std::string_view anchor)
    {
      if (_callbacks.onRouteReady)
      {
        _callbacks.onRouteReady(_callbacks.userData, anchor);
      }
    }
    void fireFormatChanged(Format const& fmt)
    {
      _format = fmt;

      if (_callbacks.onFormatChanged)
      {
        _callbacks.onFormatChanged(_callbacks.userData, fmt);
      }
    }
    void fireBackendError(std::string_view msg)
    {
      if (_callbacks.onBackendError)
      {
        _callbacks.onBackendError(_callbacks.userData, msg);
      }
    }
    void fireDrainComplete()
    {
      if (_callbacks.onDrainComplete)
      {
        _callbacks.onDrainComplete(_callbacks.userData);
      }
    }
    void firePropertyChanged(PropertyId id)
    {
      if (_callbacks.onPropertyChanged && _callbacks.userData)
      {
        _callbacks.onPropertyChanged(_callbacks.userData, id);
      }
    }

  private:
    std::vector<Event> _events;
    RenderCallbacks _callbacks{};
    Format _format{};
    ao::Result<> _openResult{};
    float _volume = 1.0f;
    bool _muted = false;
  };
} // namespace ao::audio
