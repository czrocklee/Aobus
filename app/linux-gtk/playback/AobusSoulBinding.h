// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "runtime/CorePrimitives.h"
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
    AobusSoulBinding(AobusSoul& soul, ao::rt::PlaybackService& playback);
    ~AobusSoulBinding();

    AobusSoulBinding(AobusSoulBinding const&) = delete;
    AobusSoulBinding& operator=(AobusSoulBinding const&) = delete;

  private:
    AobusSoul& _soul;
    ao::rt::PlaybackService& _playback;
    bool _playing = false;
    bool _ready = false;
    ao::audio::Quality _quality = ao::audio::Quality::Unknown;

    ao::rt::Subscription _qualitySub;
    ao::rt::Subscription _outputSub;
    ao::rt::Subscription _startedSub;
    ao::rt::Subscription _stoppedSub;
    ao::rt::Subscription _idleSub;
  };
} // namespace ao::gtk
