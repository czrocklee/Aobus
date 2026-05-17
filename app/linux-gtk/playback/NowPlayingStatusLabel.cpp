// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/NowPlayingStatusLabel.h"
#include <ao/audio/Types.h>
#include <runtime/PlaybackService.h>
#include <runtime/StateTypes.h>

#include <gdkmm/cursor.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/label.h>

#include <format>

namespace ao::gtk
{
  NowPlayingStatusLabel::NowPlayingStatusLabel(rt::PlaybackService& playbackService)
    : _playbackService{playbackService}
  {
    _label.add_css_class("ao-nowplaying");
    _label.add_css_class("ao-clickable");
    _label.set_tooltip_text("Click to show playing list");

    auto const clickGesture = Gtk::GestureClick::create();
    clickGesture->signal_pressed().connect([this](int, double, double) { _playbackService.revealPlayingTrack(); });

    _label.add_controller(clickGesture);
    _label.set_cursor(Gdk::Cursor::create("pointer"));

    _startedSub = _playbackService.onStarted([this] { updateState(); });
    _pausedSub = _playbackService.onPaused([this] { updateState(); });
    _idleSub = _playbackService.onIdle([this] { updateState(); });
    _stoppedSub = _playbackService.onStopped([this] { updateState(); });

    updateState();
  }

  NowPlayingStatusLabel::~NowPlayingStatusLabel() = default;

  void NowPlayingStatusLabel::updateState()
  {
    auto const& state = _playbackService.state();

    if (state.transport == audio::Transport::Idle)
    {
      _label.set_text("");
      return;
    }

    if (!state.trackTitle.empty())
    {
      if (!state.trackArtist.empty())
      {
        _label.set_text(std::format("{} - {}", state.trackArtist, state.trackTitle));
      }
      else
      {
        _label.set_text(state.trackTitle);
      }
    }
    else
    {
      _label.set_text("");
    }
  }
} // namespace ao::gtk
