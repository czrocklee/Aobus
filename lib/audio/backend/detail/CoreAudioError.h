// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <MacTypes.h>

#include <expected>
#include <source_location>
#include <string>
#include <string_view>

namespace ao::audio::backend::detail
{
  Error::Code coreAudioErrorCode(::OSStatus status, Error::Code fallback) noexcept;

  bool isCoreAudioDeviceLossStatus(::OSStatus status) noexcept;

  std::string coreAudioStatusText(::OSStatus status);

  std::unexpected<Error> makeCoreAudioError(::OSStatus status,
                                            std::string_view operation,
                                            Error::Code fallback,
                                            std::source_location location = std::source_location::current());
} // namespace ao::audio::backend::detail
