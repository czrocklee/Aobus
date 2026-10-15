// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "backend/detail/AlsaPcmError.h"

#include <ao/Error.h>
#include <ao/audio/Backend.h>

#include <cerrno>
#include <cstdint>

namespace ao::audio::backend::detail
{
  Error::Code alsaPcmOpenErrorCode(std::int32_t const err) noexcept
  {
    switch (err)
    {
      case -EBUSY: return Error::Code::ResourceBusy;
      case -ENODEV:
      case -ENOENT:
      case -ENXIO: return Error::Code::DeviceNotFound;
      case -ENOMEM: return Error::Code::ResourceExhausted;
      default: return Error::Code::InitFailed;
    }
  }

  BackendOpenFailure::Cause alsaHwConstraintFailureCause(std::int32_t const status) noexcept
  {
    return status == -EINVAL ? BackendOpenFailure::Cause::TrackRouteIncompatible
                             : BackendOpenFailure::Cause::OutputFailure;
  }
} // namespace ao::audio::backend::detail
