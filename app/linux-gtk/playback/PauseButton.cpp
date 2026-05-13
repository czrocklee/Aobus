// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/PauseButton.h"

namespace ao::gtk::playback
{
  PauseButton::PauseButton(ao::rt::PlaybackService& playbackService, bool showLabel, std::string const& size)
    : _playbackService{playbackService}
  {
    _button.set_has_frame(false);
    _button.add_css_class("playback-button");

    if (size == "small")
    {
      _button.add_css_class("playback-button-small");
    }
    else if (size == "large")
    {
      _button.add_css_class("playback-button-large");
    }

    _button.set_icon_name("media-playback-pause-symbolic");
    _button.set_tooltip_text("Pause");

    if (showLabel)
    {
      _button.set_label("Pause");
    }

    _button.signal_clicked().connect([this] { _playbackService.pause(); });

    auto const refreshCallback = [this] { refresh(); };
    _startedSub = _playbackService.onStarted(refreshCallback);
    _pausedSub = _playbackService.onPaused(refreshCallback);
    _idleSub = _playbackService.onIdle(refreshCallback);
    _stoppedSub = _playbackService.onStopped(refreshCallback);

    refresh();
  }

  void PauseButton::refresh()
  {
    auto const& state = _playbackService.state();
    bool const isPlaying = (state.transport == ao::audio::Transport::Playing);

    _button.set_sensitive(state.ready && isPlaying);
  }
} // namespace ao::gtk::playback
