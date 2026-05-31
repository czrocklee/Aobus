// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "playback/VolumeBar.h"
#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/playback/VolumeViewModel.h>

#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

namespace ao::gtk
{
  /**
   * @brief A composite widget for controlling playback volume.
   */
  class VolumeControl final
  {
  public:
    explicit VolumeControl(rt::PlaybackService& playbackService);
    ~VolumeControl();

    VolumeControl(VolumeControl const&) = delete;
    VolumeControl& operator=(VolumeControl const&) = delete;
    VolumeControl(VolumeControl&&) = delete;
    VolumeControl& operator=(VolumeControl&&) = delete;

    void setOrientation(Gtk::Orientation orientation);

    Gtk::Widget& widget() { return _volumeBar; }

  private:
    void applyState(uimodel::playback::VolumeViewState const& view);

    VolumeBar _volumeBar;
    bool _updating = false;
    uimodel::playback::VolumeViewModel _controller;
  };
} // namespace ao::gtk
