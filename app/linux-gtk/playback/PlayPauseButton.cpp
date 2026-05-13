// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/PlayPauseButton.h"

namespace ao::gtk::playback
{
  PlayPauseButton::PlayPauseButton(ao::rt::PlaybackService& playbackService,
                                   std::function<void()> onPlaySelection,
                                   bool showLabel,
                                   std::string const& size)
    : _playbackService{playbackService}, _onPlaySelection{std::move(onPlaySelection)}, _showLabel{showLabel}
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

    _button.signal_clicked().connect(
      [this]
      {
        auto const& state = _playbackService.state();

        if (state.transport == ao::audio::Transport::Paused)
        {
          _playbackService.resume();
        }
        else if (state.transport == ao::audio::Transport::Playing)
        {
          _playbackService.pause();
        }
        else
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
    _preparingSub = _playbackService.onPreparing(refreshCallback);

    refresh();
  }

  void PlayPauseButton::refresh()
  {
    auto const& state = _playbackService.state();
    bool const isPlaying = (state.transport == ao::audio::Transport::Playing);

    _button.set_icon_name(isPlaying ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");
    _button.set_tooltip_text(isPlaying ? "Pause" : "Play");

    if (_showLabel)
    {
      _button.set_label(isPlaying ? "Pause" : "Play");
    }

    _button.set_sensitive(state.ready);
  }
} // namespace ao::gtk::playback
