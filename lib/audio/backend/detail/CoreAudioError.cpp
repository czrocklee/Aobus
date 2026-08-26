// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CoreAudioError.h"

#include <ao/Error.h>

#include <AudioToolbox/AUComponent.h>
#include <CoreAudio/AudioHardwareBase.h>
#include <CoreAudioTypes/CoreAudioBaseTypes.h>
#include <MacTypes.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <expected>
#include <format>
#include <source_location>
#include <string>
#include <string_view>

namespace ao::audio::backend::detail
{
  Error::Code coreAudioErrorCode(::OSStatus const status, Error::Code const fallback) noexcept
  {
    switch (status)
    {
      case ::kAudioHardwareBadDeviceError:
      case ::kAudioHardwareBadObjectError: return Error::Code::DeviceNotFound;
      case ::kAudioDeviceUnsupportedFormatError:
      case ::kAudioUnitErr_FormatNotSupported: return Error::Code::FormatRejected;
      case ::kAudioUnitErr_CannotDoInCurrentContext: return Error::Code::ResourceBusy;
      case ::kAudioUnitErr_TooManyFramesToProcess: return Error::Code::ValueTooLarge;
      case ::kAudioUnitErr_Unauthorized: return Error::Code::NotSupported;
      case ::kAudioUnitErr_InvalidParameter:
      case ::kAudioUnitErr_InvalidParameterValue:
      case ::kAudio_ParamError: return Error::Code::InvalidInput;
      case ::kAudio_MemFullError: return Error::Code::ResourceExhausted;
      default: return fallback;
    }
  }

  bool isCoreAudioDeviceLossStatus(::OSStatus const status) noexcept
  {
    return status == ::kAudioHardwareBadDeviceError || status == ::kAudioHardwareBadObjectError ||
           status == ::kAudioHardwareNotRunningError;
  }

  std::string coreAudioStatusText(::OSStatus const status)
  {
    auto const raw = static_cast<std::uint32_t>(status);
    auto const bytes = std::array<char, 4>{static_cast<char>((raw >> 24U) & 0xffU),
                                           static_cast<char>((raw >> 16U) & 0xffU),
                                           static_cast<char>((raw >> 8U) & 0xffU),
                                           static_cast<char>(raw & 0xffU)};
    auto const printable =
      std::ranges::all_of(bytes, [](char const byte) { return std::isprint(static_cast<unsigned char>(byte)) != 0; });

    if (printable)
    {
      return std::format("OSStatus '{}' ({})", std::string{bytes.data(), bytes.size()}, status);
    }

    return std::format("OSStatus {}", status);
  }

  std::unexpected<Error> makeCoreAudioError(::OSStatus const status,
                                            std::string_view const operation,
                                            Error::Code const fallback,
                                            std::source_location const location)
  {
    return makeError(coreAudioErrorCode(status, fallback),
                     std::format("{} failed ({})", operation, coreAudioStatusText(status)),
                     location);
  }
} // namespace ao::audio::backend::detail
