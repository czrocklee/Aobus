// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CoreAudioUnitRun.h"

#include <ao/Contract.h>

#include <MacTypes.h>

#include <utility>

namespace ao::audio::backend::detail
{
  CoreAudioUnitRun::CoreAudioUnitRun(CoreAudioUnitRunOperations operations)
    : _operations{std::move(operations)}
  {
    AO_EXPECTS(_operations.start, "Core Audio native start operation must be bound");
    AO_EXPECTS(_operations.stop, "Core Audio native stop operation must be bound");
  }

  ::OSStatus CoreAudioUnitRun::start(::AudioUnit const unit)
  {
    AO_EXPECTS(canStart(), "Cannot start Core Audio output while a native stop is unconfirmed");

    auto const status = _operations.start(unit);

    if (status == ::noErr)
    {
      _started = true;
    }

    return status;
  }

  ::OSStatus CoreAudioUnitRun::stop(::AudioUnit const unit)
  {
    // An unstarted attachment has no native stop to confirm or retry.
    if (!_started)
    {
      return ::noErr;
    }

    auto const status = _operations.stop(unit);
    _stopStatus = status;

    if (status == ::noErr)
    {
      _started = false;
    }

    return status;
  }

  bool CoreAudioUnitRun::isStarted() const noexcept
  {
    return _started;
  }

  bool CoreAudioUnitRun::hasUnconfirmedStop() const noexcept
  {
    return _stopStatus != ::noErr;
  }

  ::OSStatus CoreAudioUnitRun::lastStopStatus() const noexcept
  {
    return _stopStatus;
  }

  bool CoreAudioUnitRun::canStart() const noexcept
  {
    return !hasUnconfirmedStop();
  }

  bool CoreAudioUnitRun::canReset() const noexcept
  {
    return !_started;
  }

  void CoreAudioUnitRun::detach() noexcept
  {
    _started = false;
    _stopStatus = ::noErr;
  }
} // namespace ao::audio::backend::detail
