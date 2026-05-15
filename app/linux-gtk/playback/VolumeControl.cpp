// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/VolumeControl.h"
#include <runtime/PlaybackService.h>
#include <runtime/StateTypes.h>

#include <gtkmm/enums.h>

namespace ao::gtk
{
  VolumeControl::VolumeControl(rt::PlaybackService& playbackService)
    : _playbackService{playbackService}
  {
    int const preferredWidth = 32;
    int const preferredHeight = 24;
    _volumeBar.set_size_request(preferredWidth, preferredHeight);
    _volumeBar.set_valign(Gtk::Align::CENTER);
    _volumeBar.set_tooltip_text("Volume");

    _volumeBar.signalVolumeChanged().connect(
      [this](float volume)
      {
        if (_updating)
        {
          return;
        }

        _playbackService.setVolume(volume);
      });

    auto const refreshCallback = [this] { refresh(); };
    _outputSub = _playbackService.onOutputChanged([refreshCallback](auto const&) { refreshCallback(); });
    _startedSub = _playbackService.onStarted(refreshCallback);

    refresh();
  }

  void VolumeControl::refresh()
  {
    auto const& state = _playbackService.state();
    _volumeBar.set_visible(state.volumeAvailable);

    if (state.volumeAvailable)
    {
      _updating = true;
      _volumeBar.setVolume(state.volume);
      _updating = false;
    }
  }
} // namespace ao::gtk
