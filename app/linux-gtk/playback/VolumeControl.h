// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "playback/VolumeBar.h"
#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/playback/VolumeViewModel.h>

#include <gtkmm/widget.h>

#include <memory>

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

    Gtk::Widget& widget() { return _volumeBar; }

  private:
    void applyState(uimodel::playback::VolumeViewState const& view);

    VolumeBar _volumeBar;
    bool _updating = false;
    std::unique_ptr<uimodel::playback::VolumeViewModel> _controllerPtr;
  };
} // namespace ao::gtk
