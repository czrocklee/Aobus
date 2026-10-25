// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/CoreAudioError.h"

#include <ao/Error.h>
#include <ao/audio/Backend.h>

#include <AudioToolbox/AUComponent.h>
#include <CoreAudio/AudioHardwareBase.h>
#include <CoreAudioTypes/CoreAudioBaseTypes.h>
#include <MacTypes.h>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <source_location>
#include <string>
#include <string_view>

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

  TEST_CASE("CoreAudioError - device loss errors retain native context independently of fallback",
            "[audio][unit][coreaudio]")
  {
    auto const statuses = std::to_array<::OSStatus>(
      {::kAudioHardwareBadDeviceError, ::kAudioHardwareBadObjectError, ::kAudioHardwareNotRunningError});
    auto const fallbacks = std::array{Error::Code::InitFailed, Error::Code::IoError};
    auto const location = std::source_location::current();

    for (auto const status : statuses)
    {
      for (auto const fallback : fallbacks)
      {
        CAPTURE(status, fallback);
        CHECK(coreAudioErrorCode(status, fallback) == Error::Code::DeviceNotFound);

        auto const error = makeCoreAudioError(status, "required channel property read", fallback, location).error();
        CHECK(error.code == Error::Code::DeviceNotFound);
        CHECK(error.message.contains("required channel property read"));
        CHECK(error.message.contains(coreAudioStatusText(status)));
        CHECK(std::string_view{error.location.file_name()} == location.file_name());
        CHECK(std::string_view{error.location.function_name()} == location.function_name());
        CHECK(error.location.line() == location.line());
        CHECK(error.location.column() == location.column());
      }
    }
  }

  TEST_CASE("CoreAudioError - arbitrary native faults preserve fallback and native context", "[audio][unit][coreaudio]")
  {
    ::OSStatus const status = -123456;
    auto const fallbacks = std::array{Error::Code::InitFailed, Error::Code::IoError};
    auto const location = std::source_location::current();
    CHECK_FALSE(isCoreAudioDeviceLossStatus(status));

    for (auto const fallback : fallbacks)
    {
      CAPTURE(fallback);
      CHECK(coreAudioErrorCode(status, fallback) == fallback);

      auto const error = makeCoreAudioError(status, "required channel property read", fallback, location).error();
      CHECK(error.code == fallback);
      CHECK(error.message.contains("required channel property read"));
      CHECK(error.message.contains("OSStatus -123456"));
      CHECK(std::string_view{error.location.file_name()} == location.file_name());
      CHECK(std::string_view{error.location.function_name()} == location.function_name());
      CHECK(error.location.line() == location.line());
      CHECK(error.location.column() == location.column());
    }
  }

  TEST_CASE("CoreAudioError - recognizes terminal device statuses", "[audio][unit][coreaudio]")
  {
    CHECK(isCoreAudioDeviceLossStatus(::kAudioHardwareBadDeviceError));
    CHECK(isCoreAudioDeviceLossStatus(::kAudioHardwareBadObjectError));
    CHECK(isCoreAudioDeviceLossStatus(::kAudioHardwareNotRunningError));
    CHECK_FALSE(isCoreAudioDeviceLossStatus(::kAudioUnitErr_FormatNotSupported));
  }

  TEST_CASE("CoreAudioError - client format set failures map to open causes", "[audio][unit][coreaudio]")
  {
    struct CauseCase final
    {
      ::OSStatus status = ::noErr;
      BackendOpenFailure::Cause cause = BackendOpenFailure::Cause::OutputFailure;
    };

    auto const causeCases =
      std::array{CauseCase{::kAudioUnitErr_FormatNotSupported, BackendOpenFailure::Cause::TrackRouteIncompatible},
                 CauseCase{::kAudioDeviceUnsupportedFormatError, BackendOpenFailure::Cause::TrackRouteIncompatible},
                 CauseCase{::kAudioHardwareBadDeviceError, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{::kAudioHardwareBadObjectError, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{::kAudioUnitErr_CannotDoInCurrentContext, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{::kAudioUnitErr_InvalidParameter, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{::kAudio_MemFullError, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{-123456, BackendOpenFailure::Cause::OutputFailure}};

    for (auto const& causeCase : causeCases)
    {
      CAPTURE(causeCase.status);
      CHECK(coreAudioClientFormatSetFailureCause(causeCase.status) == causeCase.cause);
    }
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

  TEST_CASE("CoreAudioError - unconfirmed stop text preserves status without claiming a new attempt",
            "[audio][unit][coreaudio]")
  {
    CHECK(coreAudioUnconfirmedStopText(-123456) == "previous native stop remains unconfirmed (OSStatus -123456)");
    CHECK(coreAudioUnconfirmedStopText(1937010544) ==
          "previous native stop remains unconfirmed (OSStatus 'stop' (1937010544))");
  }

  TEST_CASE("CoreAudioError - preserves the owning call site alongside native operation context",
            "[audio][unit][coreaudio]")
  {
    auto const location = std::source_location::current();
    auto const error =
      makeCoreAudioError(::kAudioUnitErr_FormatNotSupported, "required map readback", Error::Code::InitFailed, location)
        .error();
    CHECK(error.code == Error::Code::FormatRejected);
    CHECK(error.message.contains("required map readback"));
    CHECK(error.message.contains(coreAudioStatusText(::kAudioUnitErr_FormatNotSupported)));
    CHECK(std::string_view{error.location.file_name()} == location.file_name());
    CHECK(std::string_view{error.location.function_name()} == location.function_name());
    CHECK(error.location.line() == location.line());
    CHECK(error.location.column() == location.column());
  }
} // namespace ao::audio::backend::detail::test
