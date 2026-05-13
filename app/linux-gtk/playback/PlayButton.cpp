// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/PlayButton.h"

namespace ao::gtk::playback
{
  PlayButton::PlayButton(ao::rt::PlaybackService& playbackService,
                         std::function<void()> onPlaySelection,
                         bool showLabel,
                         std::string const& size)
    : _playbackService{playbackService}, _onPlaySelection{std::move(onPlaySelection)}
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

    _button.set_icon_name("media-playback-start-symbolic");
    _button.set_tooltip_text("Play");

    if (showLabel)
    {
      _button.set_label("Play");
    }

    _button.signal_clicked().connect(
      [this]
      {
        auto const& state = _playbackService.state();

        if (state.transport == ao::audio::Transport::Paused)
        {
          _playbackService.resume();
        }
        else if (state.transport != ao::audio::Transport::Playing)
        {
          if (_onPlaySelection)
          {
            _onPlaySelection();
          }
        }
      });

    auto const refreshCallback = [this] { refresh(); };
    _startedSub = _playbackService.onStarted(refreshCallback);
    _pausedSub = _playbackService.onPaused(refreshCallback);
    _idleSub = _playbackService.onIdle(refreshCallback);
    _stoppedSub = _playbackService.onStopped(refreshCallback);

    refresh();
  }

  void PlayButton::refresh()
  {
    auto const& state = _playbackService.state();
    bool const isPlaying = (state.transport == ao::audio::Transport::Playing);

    _button.set_sensitive(state.ready && !isPlaying);
  }
} // namespace ao::gtk::playback
