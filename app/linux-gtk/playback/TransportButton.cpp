// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/TransportButton.h"

namespace ao::gtk
{
  namespace
  {
    char const* iconForAction(TransportButton::Action action)
    {
      switch (action)
      {
        case TransportButton::Action::Play: return "media-playback-start-symbolic";
        case TransportButton::Action::Pause: return "media-playback-pause-symbolic";
        case TransportButton::Action::Stop: return "media-playback-stop-symbolic";
        case TransportButton::Action::PlayPause: return "media-playback-start-symbolic";
      }
      return "media-playback-start-symbolic";
    }

    char const* labelForAction(TransportButton::Action action)
    {
      switch (action)
      {
        case TransportButton::Action::Play: return "Play";
        case TransportButton::Action::Pause: return "Pause";
        case TransportButton::Action::Stop: return "Stop";
        case TransportButton::Action::PlayPause: return "Play";
      }
      return "";
    }

    void applySizeClass(Gtk::Button& button, std::string const& size)
    {
      if (size == "small")
      {
        button.add_css_class("playback-button-small");
      }
      else if (size == "large")
      {
        button.add_css_class("playback-button-large");
      }
    }
  } // namespace

  TransportButton::TransportButton(rt::PlaybackService& playbackService,
                                   Action action,
                                   std::function<void()> onPlaySelection,
                                   bool showLabel,
                                   std::string const& size)
    : _playbackService{playbackService}
    , _action{action}
    , _onPlaySelection{std::move(onPlaySelection)}
    , _showLabel{showLabel}
  {
    _button.set_has_frame(false);
    _button.add_css_class("playback-button");
    applySizeClass(_button, size);

    _button.set_icon_name(iconForAction(action));
    _button.set_tooltip_text(labelForAction(action));

    if (showLabel)
    {
      _button.set_label(labelForAction(action));
    }

    _button.signal_clicked().connect(
      [this]
      {
        auto const& state = _playbackService.state();

        switch (_action)
        {
          case Action::Play:
            if (state.transport == audio::Transport::Paused)
            {
              _playbackService.resume();
            }
            else if (state.transport != audio::Transport::Playing)
            {
              if (_onPlaySelection)
              {
                _onPlaySelection();
              }
            }
            break;

          case Action::Pause: _playbackService.pause(); break;

          case Action::Stop: _playbackService.stop(); break;

          case Action::PlayPause:
            if (state.transport == audio::Transport::Paused)
            {
              _playbackService.resume();
            }
            else if (state.transport == audio::Transport::Playing)
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
            break;
        }
      });

    auto const refreshCallback = [this] { refresh(); };
    _startedSub = _playbackService.onStarted(refreshCallback);
    _pausedSub = _playbackService.onPaused(refreshCallback);
    _idleSub = _playbackService.onIdle(refreshCallback);
    _stoppedSub = _playbackService.onStopped(refreshCallback);

    if (_action == Action::PlayPause)
    {
      _preparingSub = _playbackService.onPreparing(refreshCallback);
    }

    refresh();
  }

  void TransportButton::refresh()
  {
    auto const& state = _playbackService.state();
    bool const isPlaying = (state.transport == audio::Transport::Playing);

    switch (_action)
    {
      case Action::Play: _button.set_sensitive(state.ready && !isPlaying); break;

      case Action::Pause: _button.set_sensitive(state.ready && isPlaying); break;

      case Action::Stop: _button.set_sensitive(state.transport != audio::Transport::Idle); break;

      case Action::PlayPause:
        _button.set_icon_name(isPlaying ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");
        _button.set_tooltip_text(isPlaying ? "Pause" : "Play");
        if (_showLabel)
        {
          _button.set_label(isPlaying ? "Pause" : "Play");
        }
        _button.set_sensitive(state.ready);
        break;
    }
  }
} // namespace ao::gtk
