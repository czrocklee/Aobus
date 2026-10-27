// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/backend/detail/AlsaPcmError.h"

#include <ao/Error.h>
#include <ao/audio/Backend.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <cstdint>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("AlsaPcmError - classifies terminal stream errors")
  {
    CHECK(isUnrecoverableAlsaPcmError(-ENODEV));
    CHECK(isUnrecoverableAlsaPcmError(-EBADF));

    CHECK_FALSE(isUnrecoverableAlsaPcmError(-EPIPE));
    CHECK_FALSE(isUnrecoverableAlsaPcmError(-ESTRPIPE));
    CHECK_FALSE(isUnrecoverableAlsaPcmError(-EAGAIN));
  }

  TEST_CASE("AlsaPcmError - reports device contention distinctly")
  {
    CHECK(alsaPcmOpenErrorCode(-EBUSY) == Error::Code::ResourceBusy);
    CHECK(alsaPcmOpenErrorCode(-ENODEV) == Error::Code::DeviceNotFound);
    CHECK(alsaPcmOpenErrorCode(-ENOENT) == Error::Code::DeviceNotFound);
    CHECK(alsaPcmOpenErrorCode(-ENXIO) == Error::Code::DeviceNotFound);
    CHECK(alsaPcmOpenErrorCode(-ENOMEM) == Error::Code::ResourceExhausted);
    CHECK(alsaPcmOpenErrorCode(-EACCES) == Error::Code::InitFailed);
    CHECK(alsaPcmOpenErrorCode(-EINVAL) == Error::Code::InitFailed);
  }

  TEST_CASE("alsaHwConstraintFailureCause - only exact-constraint rejection routes to the track")
  {
    struct CauseCase final
    {
      std::int32_t status = 0;
      BackendOpenFailure::Cause cause = BackendOpenFailure::Cause::OutputFailure;
    };

    auto const cases = std::array{CauseCase{-EINVAL, BackendOpenFailure::Cause::TrackRouteIncompatible},
                                  CauseCase{-ENODEV, BackendOpenFailure::Cause::OutputFailure},
                                  CauseCase{-EBADFD, BackendOpenFailure::Cause::OutputFailure},
                                  CauseCase{-EBADF, BackendOpenFailure::Cause::OutputFailure},
                                  CauseCase{-EIO, BackendOpenFailure::Cause::OutputFailure},
                                  CauseCase{-ENOMEM, BackendOpenFailure::Cause::OutputFailure},
                                  CauseCase{-EBUSY, BackendOpenFailure::Cause::OutputFailure},
                                  CauseCase{-EAGAIN, BackendOpenFailure::Cause::OutputFailure},
                                  CauseCase{-ESTRPIPE, BackendOpenFailure::Cause::OutputFailure},
                                  CauseCase{-123456, BackendOpenFailure::Cause::OutputFailure},
                                  CauseCase{0, BackendOpenFailure::Cause::OutputFailure}};

    for (auto const& [status, expectedCause] : cases)
    {
      CAPTURE(status);
      CHECK(alsaHwConstraintFailureCause(status) == expectedCause);
    }
  }
} // namespace ao::audio::backend::detail::test
