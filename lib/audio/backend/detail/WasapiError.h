// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// audioclient.h assumes the base Win32 types and pointer macros from windows.h.
// clang-format off
#include <windows.h>
#include <audioclient.h>
// clang-format on

namespace ao::audio::backend::detail
{
  /**
   * @brief Classifies only a failed shared-mode IAudioClient::Initialize call.
   *
   * An explicit audio-engine format rejection leaves the track's route
   * incompatible; every other status, including unknown ones, is a terminal
   * output failure. Other HRESULT classifications do not use this helper.
   */
  constexpr BackendOpenFailure::Cause wasapiInitializeFailureCause(::HRESULT status) noexcept
  {
    return status == AUDCLNT_E_UNSUPPORTED_FORMAT ? BackendOpenFailure::Cause::TrackRouteIncompatible
                                                  : BackendOpenFailure::Cause::OutputFailure;
  }
} // namespace ao::audio::backend::detail
