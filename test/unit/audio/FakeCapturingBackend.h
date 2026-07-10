// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Format.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>

#include <expected>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  class FakeCapturingBackend final : public Backend
  {
  public:
    struct Event final
    {
      std::string name;
      Format format;
    };

    Result<> open(Format const& format, RenderTarget* target) override
    {
      auto const lock = std::scoped_lock{_mutex};
      recordEvent("open", format);
      _target = target;
      _format = format;
      return _openResult;
    }

    void start() override
    {
      auto const lock = std::scoped_lock{_mutex};
      recordEvent("start", {});
    }
    void pause() override
    {
      auto const lock = std::scoped_lock{_mutex};
      recordEvent("pause", {});
    }
    void resume() override
    {
      auto const lock = std::scoped_lock{_mutex};
      recordEvent("resume", {});
    }
    void flush() override
    {
      auto const lock = std::scoped_lock{_mutex};
      recordEvent("flush", {});
    }
    void stop() override
    {
      auto const lock = std::scoped_lock{_mutex};
      recordEvent("stop", {});
    }
    void close() override
    {
      auto const lock = std::scoped_lock{_mutex};
      recordEvent("close", {});
      _target = nullptr;
    }

    BackendId backendId() const override { return BackendId{"capturing"}; }
    ProfileId profileId() const override { return ProfileId{"test"}; }

    Result<> setProperty(PropertyId id, PropertyValue const& value) override
    {
      auto const lock = std::scoped_lock{_mutex};

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

      return std::unexpected(Error{.code = Error::Code::NotSupported});
    }

    Result<PropertyValue> property(PropertyId id) const override
    {
      auto const lock = std::scoped_lock{_mutex};

      if (_optPropError)
      {
        return std::unexpected(Error{.code = *_optPropError});
      }

      if (id == PropertyId::Volume)
      {
        return _volume;
      }

      if (id == PropertyId::Muted)
      {
        return _muted;
      }

      return std::unexpected(Error{.code = Error::Code::NotSupported});
    }

    PropertyInfo queryProperty(PropertyId id) const noexcept override
    {
      auto const lock = std::scoped_lock{_mutex};

      return propertyInfoUnlocked(id);
    }

    // Helpers for tests
    void setMockPropertyInfo(PropertyId id, PropertyInfo const& info)
    {
      auto const lock = std::scoped_lock{_mutex};
      _mockPropertyInfos[id] = info;
    }
    void setOpenResult(Result<> res)
    {
      auto const lock = std::scoped_lock{_mutex};
      _openResult = res;
    }
    void setPropertyError(std::optional<Error::Code> optErr)
    {
      auto const lock = std::scoped_lock{_mutex};
      _optPropError = optErr;
    }
    std::vector<Event> events() const
    {
      auto const lock = std::scoped_lock{_mutex};
      return _events;
    }
    void clearEvents()
    {
      auto const lock = std::scoped_lock{_mutex};
      _events.clear();
    }
    void setEventObserver(std::function<void(std::string_view)> observer)
    {
      auto const lock = std::scoped_lock{_mutex};
      _eventObserver = std::move(observer);
    }
    RenderTarget* target() const
    {
      auto const lock = std::scoped_lock{_mutex};
      return _target;
    }
    Format currentFormat() const
    {
      auto const lock = std::scoped_lock{_mutex};
      return _format;
    }

    // Trigger callbacks
    void emitRouteReady(std::string_view anchor)
    {
      RenderTarget* t = nullptr;
      {
        auto const lock = std::scoped_lock{_mutex};
        t = _target;
      }

      if (t != nullptr)
      {
        t->handleRouteReady(anchor);
      }
    }

    void emitFormatChanged(Format const& fmt)
    {
      RenderTarget* t = nullptr;
      {
        auto const lock = std::scoped_lock{_mutex};
        _format = fmt;
        t = _target;
      }

      if (t != nullptr)
      {
        t->handleFormatChanged(fmt);
      }
    }

    void emitBackendError(std::string_view msg)
    {
      RenderTarget* t = nullptr;
      {
        auto const lock = std::scoped_lock{_mutex};
        t = _target;
      }

      if (t != nullptr)
      {
        t->handleBackendError(msg);
      }
    }

    void emitDrainComplete()
    {
      RenderTarget* t = nullptr;
      {
        auto const lock = std::scoped_lock{_mutex};
        t = _target;
      }

      if (t != nullptr)
      {
        t->handleDrainComplete();
      }
    }

    void emitPropertyChanged(PropertyId id)
    {
      RenderTarget* t = nullptr;
      auto snapshot = PropertySnapshot{};
      {
        auto const lock = std::scoped_lock{_mutex};
        t = _target;
        snapshot = propertySnapshotUnlocked(id);
      }

      if (t != nullptr)
      {
        t->handlePropertyChanged(std::move(snapshot));
      }
    }

  private:
    PropertyInfo propertyInfoUnlocked(PropertyId id) const
    {
      if (auto const it = _mockPropertyInfos.find(id); it != _mockPropertyInfos.end())
      {
        return it->second;
      }

      if (id == PropertyId::Volume || id == PropertyId::Muted)
      {
        return {.canRead = true, .canWrite = true, .isAvailable = true, .emitsChangeNotifications = false};
      }

      return {};
    }

    PropertySnapshot propertySnapshotUnlocked(PropertyId id) const
    {
      auto optValue = std::optional<PropertyValue>{};

      if (!_optPropError)
      {
        if (id == PropertyId::Volume)
        {
          optValue = _volume;
        }
        else if (id == PropertyId::Muted)
        {
          optValue = _muted;
        }
      }

      return PropertySnapshot{
        .id = id,
        .optValue = std::move(optValue),
        .info = propertyInfoUnlocked(id),
      };
    }

    void recordEvent(std::string_view name, Format const& format)
    {
      _events.push_back({.name = std::string{name}, .format = format});

      if (_eventObserver)
      {
        _eventObserver(name);
      }
    }

    mutable std::mutex _mutex;
    std::vector<Event> _events;
    std::function<void(std::string_view)> _eventObserver;
    RenderTarget* _target = nullptr;
    Format _format{};
    Result<> _openResult{};
    std::optional<Error::Code> _optPropError{};
    std::map<PropertyId, PropertyInfo> _mockPropertyInfos{};
    float _volume = 1.0F;
    bool _muted = false;
  };
} // namespace ao::audio::test
