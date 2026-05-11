// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

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

    ao::Result<> open(Format const& format, IRenderTarget* target) override
    {
      _events.push_back({"open", format});
      _target = target;
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
    IRenderTarget* target() const { return _target; }
    Format currentFormat() const { return _format; }

    // Trigger callbacks
    void fireRouteReady(std::string_view anchor)
    {
      if (_target)
      {
        _target->onRouteReady(anchor);
      }
    }

    void fireFormatChanged(Format const& fmt)
    {
      _format = fmt;

      if (_target)
      {
        _target->onFormatChanged(fmt);
      }
    }

    void fireBackendError(std::string_view msg)
    {
      if (_target)
      {
        _target->onBackendError(msg);
      }
    }

    void fireDrainComplete()
    {
      if (_target)
      {
        _target->onDrainComplete();
      }
    }

    void firePropertyChanged(PropertyId id)
    {
      if (_target)
      {
        _target->onPropertyChanged(id);
      }
    }

  private:
    std::vector<Event> _events;
    IRenderTarget* _target = nullptr;
    Format _format{};
    ao::Result<> _openResult{};
    float _volume = 1.0f;
    bool _muted = false;
  };
} // namespace ao::audio
