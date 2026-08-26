// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/CoreAudioError.h"

#include <ao/Error.h>

#include <AudioToolbox/AUComponent.h>
#include <CoreAudio/AudioHardwareBase.h>
#include <catch2/catch_test_macros.hpp>

#include <string>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("CoreAudioError - classifies native failures", "[audio][unit][coreaudio]")
  {
    CHECK(coreAudioErrorCode(::kAudioHardwareBadDeviceError, Error::Code::IoError) == Error::Code::DeviceNotFound);
    CHECK(coreAudioErrorCode(::kAudioUnitErr_FormatNotSupported, Error::Code::InitFailed) ==
          Error::Code::FormatRejected);
    CHECK(coreAudioErrorCode(::kAudioUnitErr_CannotDoInCurrentContext, Error::Code::InitFailed) ==
          Error::Code::ResourceBusy);
    CHECK(coreAudioErrorCode(-123456, Error::Code::IoError) == Error::Code::IoError);
  }

  TEST_CASE("CoreAudioError - recognizes terminal device statuses", "[audio][unit][coreaudio]")
  {
    CHECK(isCoreAudioDeviceLossStatus(::kAudioHardwareBadDeviceError));
    CHECK(isCoreAudioDeviceLossStatus(::kAudioHardwareBadObjectError));
    CHECK(isCoreAudioDeviceLossStatus(::kAudioHardwareNotRunningError));
    CHECK_FALSE(isCoreAudioDeviceLossStatus(::kAudioUnitErr_FormatNotSupported));
  }

  TEST_CASE("CoreAudioError - describes printable and numeric OSStatus values", "[audio][unit][coreaudio]")
  {
    CHECK(coreAudioStatusText(::kAudioHardwareBadDeviceError).contains("!dev"));
    CHECK(coreAudioStatusText(-123456) == "OSStatus -123456");

    auto const error =
      makeCoreAudioError(::kAudioHardwareBadDeviceError, "select Core Audio device", Error::Code::InitFailed);
    CHECK(error.error().code == Error::Code::DeviceNotFound);
    CHECK(error.error().message.contains("select Core Audio device"));
  }
} // namespace ao::audio::backend::detail::test
