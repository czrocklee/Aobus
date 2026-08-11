// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/NullBackend.h>

#include "detail/DecoderOutput.h"
#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/SignalFormat.h>

#include <expected>

namespace ao::audio
{
  NullBackend::NullBackend() noexcept = default;
  NullBackend::~NullBackend() = default;

  Result<OpenedPcmMode> NullBackend::open(SignalFormat const& sourceFormat, RenderTarget& /*target*/)
  {
    auto const encodings = detail::losslessPcmEncodings(sourceFormat);

    if (encodings.empty())
    {
      return makeError(Error::Code::NotSupported, "No lossless PCM encoding is available");
    }

    // A sink that discards frames has no endpoint to confirm, so it stays on
    // the lossless path and never authorizes a precision reduction.
    return OpenedPcmMode{.clientFormat = pcmFormat(sourceFormat, encodings.front())};
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
