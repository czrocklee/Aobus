// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/Property.h>

#include <expected>

namespace ao::audio
{
  NullBackend::NullBackend() noexcept = default;
  NullBackend::~NullBackend() = default;

  Result<> NullBackend::open(Format const& /*format*/, RenderTarget* /*target*/)
  {
    return {};
  }

  void NullBackend::start()
  {
  }
  void NullBackend::pause()
  {
  }
  void NullBackend::resume()
  {
  }
  void NullBackend::flush()
  {
  }
  void NullBackend::stop()
  {
  }
  void NullBackend::close()
  {
  }

  Result<> NullBackend::setProperty(PropertyId id, PropertyValue const& value)
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

    return std::unexpected(Error{.code = Error::Code::NotSupported});
  }

  Result<PropertyValue> NullBackend::property(PropertyId id) const
  {
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

  PropertyInfo NullBackend::queryProperty(PropertyId id) const noexcept
  {
    if (id == PropertyId::Volume || id == PropertyId::Muted)
    {
      return {.canRead = true, .canWrite = true, .isAvailable = true, .emitsChangeNotifications = false};
    }

    return {};
  }

  BackendId NullBackend::backendId() const
  {
    return BackendId{kBackendNone};
  }

  ProfileId NullBackend::profileId() const
  {
    return ProfileId{kProfileShared};
  }
} // namespace ao::audio
