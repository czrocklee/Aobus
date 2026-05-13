// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "playback/VolumeBar.h"
#include "runtime/PlaybackService.h"

namespace ao::gtk::playback
{
  /**
   * @brief A composite widget for controlling playback volume.
   */
  class VolumeControl final
  {
  public:
    explicit VolumeControl(ao::rt::PlaybackService& playbackService);

    Gtk::Widget& widget() { return _volumeBar; }

  private:
    void refresh();

    ao::rt::PlaybackService& _playbackService;
    ao::gtk::VolumeBar _volumeBar;
    bool _updating = false;

    ao::rt::Subscription _outputSub;
    ao::rt::Subscription _startedSub;
  };
} // namespace ao::gtk::playback
