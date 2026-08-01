// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/audio/backend/detail/AlsaPcmError.h>

#include <ao/Error.h>

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
} // namespace ao::audio::backend::detail
