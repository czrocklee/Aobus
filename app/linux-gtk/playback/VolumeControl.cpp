// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/VolumeControl.h"

#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/playback/VolumeViewModel.h>

#include <gtkmm/enums.h>

namespace ao::gtk
{
  VolumeControl::VolumeControl(rt::PlaybackService& playbackService)
    : _controller{playbackService, [this](ao::uimodel::playback::VolumeViewState const& state) { applyState(state); }}
  {
    _volumeBar.set_valign(Gtk::Align::CENTER);
    _volumeBar.set_tooltip_text("Volume");

    _volumeBar.signalVolumeChanged().connect(
      [this](float volume)
      {
        if (_updating)
        {
          return;
        }

        _controller.handleVolumeChanged(volume);
      });
  }

  VolumeControl::~VolumeControl() = default;

  void VolumeControl::applyState(ao::uimodel::playback::VolumeViewState const& view)
  {
    _volumeBar.set_visible(view.visible);

    if (view.visible)
    {
      _updating = true;
      _volumeBar.setVolume(view.volume);
      _updating = false;
    }
  }
} // namespace ao::gtk
