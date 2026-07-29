// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/audio/FakeCapturingBackend.h"

#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>

#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  FakeCapturingBackend::FakeCapturingBackend() = default;
  FakeCapturingBackend::~FakeCapturingBackend() = default;

  Result<> FakeCapturingBackend::open(Format const& format, RenderTarget* target)
  {
    auto const lock = std::scoped_lock{_mutex};
    recordEvent("open", format);
    _target = target;
    _format = format;
    return _openResult;
  }

  void FakeCapturingBackend::start()
  {
    auto const lock = std::scoped_lock{_mutex};
    recordEvent("start", {});
  }

  void FakeCapturingBackend::pause()
  {
    auto const lock = std::scoped_lock{_mutex};
    recordEvent("pause", {});
  }

  void FakeCapturingBackend::resume()
  {
    auto const lock = std::scoped_lock{_mutex};
    recordEvent("resume", {});
  }

  void FakeCapturingBackend::flush()
  {
    auto const lock = std::scoped_lock{_mutex};
    recordEvent("flush", {});
  }

  void FakeCapturingBackend::stop()
  {
    auto const lock = std::scoped_lock{_mutex};
    recordEvent("stop", {});
  }

  void FakeCapturingBackend::close()
  {
    auto const lock = std::scoped_lock{_mutex};
    recordEvent("close", {});
    _target = nullptr;
  }

  BackendId FakeCapturingBackend::backendId() const
  {
    return BackendId{"capturing"};
  }

  ProfileId FakeCapturingBackend::profileId() const
  {
    return ProfileId{"test"};
  }

  Result<> FakeCapturingBackend::setProperty(PropertyId id, PropertyValue const& value)
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

  Result<PropertyValue> FakeCapturingBackend::property(PropertyId id) const
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

  PropertyInfo FakeCapturingBackend::queryProperty(PropertyId id) const noexcept
  {
    auto const lock = std::scoped_lock{_mutex};

    return propertyInfoUnlocked(id);
  }

  void FakeCapturingBackend::setMockPropertyInfo(PropertyId id, PropertyInfo const& info)
  {
    auto const lock = std::scoped_lock{_mutex};
    _mockPropertyInfos[id] = info;
  }

  void FakeCapturingBackend::setOpenResult(Result<> res)
  {
    auto const lock = std::scoped_lock{_mutex};
    _openResult = res;
  }

  void FakeCapturingBackend::setPropertyError(std::optional<Error::Code> optErr)
  {
    auto const lock = std::scoped_lock{_mutex};
    _optPropError = optErr;
  }

  std::vector<FakeCapturingBackend::Event> FakeCapturingBackend::events() const
  {
    auto const lock = std::scoped_lock{_mutex};
    return _events;
  }

  void FakeCapturingBackend::clearEvents()
  {
    auto const lock = std::scoped_lock{_mutex};
    _events.clear();
  }

  void FakeCapturingBackend::setEventObserver(std::function<void(std::string_view)> observer)
  {
    auto const lock = std::scoped_lock{_mutex};
    _eventObserver = std::move(observer);
  }

  RenderTarget* FakeCapturingBackend::target() const
  {
    auto const lock = std::scoped_lock{_mutex};
    return _target;
  }

  Format FakeCapturingBackend::currentFormat() const
  {
    auto const lock = std::scoped_lock{_mutex};
    return _format;
  }

  void FakeCapturingBackend::emitRouteReady(std::string_view anchor)
  {
    RenderTarget* target = nullptr;
    {
      auto const lock = std::scoped_lock{_mutex};
      target = _target;
    }

    if (target != nullptr)
    {
      target->handleRouteReady(anchor);
    }
  }

  void FakeCapturingBackend::emitFormatChanged(Format const& format)
  {
    RenderTarget* target = nullptr;
    {
      auto const lock = std::scoped_lock{_mutex};
      _format = format;
      target = _target;
    }

    if (target != nullptr)
    {
      target->handleFormatChanged(format);
    }
  }

  void FakeCapturingBackend::emitBackendError(std::string_view message)
  {
    RenderTarget* target = nullptr;
    {
      auto const lock = std::scoped_lock{_mutex};
      target = _target;
    }

    if (target != nullptr)
    {
      target->handleBackendError(message);
    }
  }

  void FakeCapturingBackend::emitDrainComplete()
  {
    RenderTarget* target = nullptr;
    {
      auto const lock = std::scoped_lock{_mutex};
      target = _target;
    }

    if (target != nullptr)
    {
      target->handleDrainComplete();
    }
  }

  void FakeCapturingBackend::emitPropertyChanged(PropertyId id)
  {
    RenderTarget* target = nullptr;
    auto snapshot = PropertySnapshot{};
    {
      auto const lock = std::scoped_lock{_mutex};
      target = _target;
      snapshot = propertySnapshotUnlocked(id);
    }

    if (target != nullptr)
    {
      target->handlePropertyChanged(std::move(snapshot));
    }
  }

  PropertyInfo FakeCapturingBackend::propertyInfoUnlocked(PropertyId id) const
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

  PropertySnapshot FakeCapturingBackend::propertySnapshotUnlocked(PropertyId id) const
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

  void FakeCapturingBackend::recordEvent(std::string_view name, Format const& format)
  {
    _events.push_back({.name = std::string{name}, .format = format});

    if (_eventObserver)
    {
      _eventObserver(name);
    }
  }
} // namespace ao::audio::test
