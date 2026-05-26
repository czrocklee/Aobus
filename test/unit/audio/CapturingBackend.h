// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "ao/Error.h"
#include "ao/audio/Backend.h"
#include "ao/audio/Format.h"
#include "ao/audio/IBackend.h"
#include "ao/audio/IRenderTarget.h"
#include "ao/audio/Property.h"

#include <expected>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::audio::test
{
  class CapturingBackend final : public IBackend
  {
  public:
    struct Event final
    {
      std::string name;
      Format format;
    };

    Result<> open(Format const& format, IRenderTarget* target) override
    {
      auto const lock = std::scoped_lock{_mutex};
      _events.push_back({"open", format});
      _target = target;
      _format = format;
      return _openResult;
    }

    void start() override
    {
      auto const lock = std::scoped_lock{_mutex};
      _events.push_back({"start", {}});
    }
    void pause() override
    {
      auto const lock = std::scoped_lock{_mutex};
      _events.push_back({"pause", {}});
    }
    void resume() override
    {
      auto const lock = std::scoped_lock{_mutex};
      _events.push_back({"resume", {}});
    }
    void flush() override
    {
      auto const lock = std::scoped_lock{_mutex};
      _events.push_back({"flush", {}});
    }
    void stop() override
    {
      auto const lock = std::scoped_lock{_mutex};
      _events.push_back({"stop", {}});
    }
    void close() override
    {
      auto const lock = std::scoped_lock{_mutex};
      _events.push_back({"close", {}});
    }

    BackendId backendId() const noexcept override { return BackendId{"capturing"}; }
    ProfileId profileId() const noexcept override { return ProfileId{"test"}; }

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

    Result<PropertyValue> getProperty(PropertyId id) const override
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
      if (id == PropertyId::Volume || id == PropertyId::Muted)
      {
        return {.canRead = true, .canWrite = true, .isAvailable = true, .emitsChangeNotifications = false};
      }

      return {};
    }

    // Helpers for tests
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
    IRenderTarget* target() const
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
    void fireRouteReady(std::string_view anchor)
    {
      auto* t = (IRenderTarget*)nullptr;
      {
        auto const lock = std::scoped_lock{_mutex};
        t = _target;
      }

      if (t != nullptr)
      {
        t->onRouteReady(anchor);
      }
    }

    void fireFormatChanged(Format const& fmt)
    {
      auto* t = (IRenderTarget*)nullptr;
      {
        auto const lock = std::scoped_lock{_mutex};
        _format = fmt;
        t = _target;
      }

      if (t != nullptr)
      {
        t->onFormatChanged(fmt);
      }
    }

    void fireBackendError(std::string_view msg)
    {
      auto* t = (IRenderTarget*)nullptr;
      {
        auto const lock = std::scoped_lock{_mutex};
        t = _target;
      }

      if (t != nullptr)
      {
        t->onBackendError(msg);
      }
    }

    void fireDrainComplete()
    {
      auto* t = (IRenderTarget*)nullptr;
      {
        auto const lock = std::scoped_lock{_mutex};
        t = _target;
      }

      if (t != nullptr)
      {
        t->onDrainComplete();
      }
    }

    void firePropertyChanged(PropertyId id)
    {
      auto* t = (IRenderTarget*)nullptr;
      {
        auto const lock = std::scoped_lock{_mutex};
        t = _target;
      }

      if (t != nullptr)
      {
        t->onPropertyChanged(id);
      }
    }

  private:
    mutable std::mutex _mutex;
    std::vector<Event> _events;
    IRenderTarget* _target = nullptr;
    Format _format{};
    Result<> _openResult{};
    std::optional<Error::Code> _optPropError{};
    float _volume = 1.0F;
    bool _muted = false;
  };
} // namespace ao::audio::test
