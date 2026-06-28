// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/playback/output/VolumeViewModel.h>

#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <gtkmm/scale.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/widget.h>
#include <sigc++/connection.h>

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

    Gtk::Widget& widget() { return _button; }

  private:
    void applyState(uimodel::VolumeViewState const& view);

    Gtk::Button _button;
    Gtk::Image _icon;
    Gtk::Popover _popover;
    Gtk::Scale _scale{Gtk::Orientation::VERTICAL};
    Gtk::Label _valueLabel;
    Gtk::ToggleButton _muteButton;
    Gtk::Popover _scrollBubble;
    Gtk::Label _scrollBubbleLabel;

    sigc::connection _scrollBubbleTimeout;

    bool _updating = false;
    uimodel::VolumeViewModel _controller;
  };
} // namespace ao::gtk
