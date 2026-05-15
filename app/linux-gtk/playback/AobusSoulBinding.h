// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "runtime/CorePrimitives.h"
#include <ao/audio/Backend.h>
#include <ao/audio/Types.h>

namespace ao::rt
{
  class PlaybackService;
}

namespace ao::gtk
{
  class AobusSoul;

  class AobusSoulBinding final
  {
  public:
    AobusSoulBinding(AobusSoul& soul, rt::PlaybackService& playback);
    ~AobusSoulBinding();

    AobusSoulBinding(AobusSoulBinding const&) = delete;
    AobusSoulBinding& operator=(AobusSoulBinding const&) = delete;
    AobusSoulBinding(AobusSoulBinding&&) = delete;
    AobusSoulBinding& operator=(AobusSoulBinding&&) = delete;

  private:
    AobusSoul& _soul;
    rt::PlaybackService& _playback;
    bool _playing = false;
    bool _ready = false;
    audio::Quality _quality = audio::Quality::Unknown;

    rt::Subscription _qualitySub;
    rt::Subscription _outputSub;
    rt::Subscription _startedSub;
    rt::Subscription _stoppedSub;
    rt::Subscription _idleSub;
  };
} // namespace ao::gtk
