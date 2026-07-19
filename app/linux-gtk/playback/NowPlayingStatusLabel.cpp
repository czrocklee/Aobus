// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/NowPlayingStatusLabel.h"

#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>

#include <gdkmm/cursor.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/label.h>

#include <cstdint>

namespace ao::gtk
{
  NowPlayingStatusLabel::NowPlayingStatusLabel(rt::PlaybackService& playback)
    : _commands{playback.commands()}
    , _nowPlayingViewModel{playback, [this](ao::uimodel::NowPlayingViewState const& view) { applyState(view); }}
  {
    _label.add_css_class("ao-nowplaying");
    _label.add_css_class("ao-clickable");
    _label.set_tooltip_text("Click to show playing list");

    auto const clickGesturePtr = Gtk::GestureClick::create();
    clickGesturePtr->signal_pressed().connect([this](std::int32_t, double, double) { _commands.revealPlayingTrack(); });

    _label.add_controller(clickGesturePtr);
    _label.set_cursor(Gdk::Cursor::create("pointer"));
  }

  NowPlayingStatusLabel::~NowPlayingStatusLabel() = default;

  void NowPlayingStatusLabel::applyState(ao::uimodel::NowPlayingViewState const& view)
  {
    _label.set_text(view.combinedStatus);
  }
} // namespace ao::gtk
