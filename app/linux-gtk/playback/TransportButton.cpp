// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/TransportButton.h"

#include "ao/audio/Types.h"
#include "playback/PlaybackSequenceController.h"
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>

#include <gtkmm/button.h>
#include <gtkmm/enums.h>

#include <functional>
#include <string>
#include <utility>

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
        case TransportButton::Action::Next: return "media-skip-forward-symbolic";
        case TransportButton::Action::Previous: return "media-skip-backward-symbolic";
        case TransportButton::Action::Shuffle: return "media-playlist-shuffle-symbolic";
        case TransportButton::Action::Repeat: return "media-playlist-repeat-symbolic";
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
        case TransportButton::Action::Next: return "Next Track";
        case TransportButton::Action::Previous: return "Previous Track";
        case TransportButton::Action::Shuffle: return "Shuffle";
        case TransportButton::Action::Repeat: return "Repeat";
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
                                   PlaybackSequenceController* sequenceController,
                                   Action action,
                                   std::function<void()> onPlaySelection,
                                   bool showLabel,
                                   std::string const& size)
    : _playbackService{playbackService}
    , _sequenceController{sequenceController}
    , _action{action}
    , _onPlaySelection{std::move(onPlaySelection)}
    , _showLabel{showLabel}
  {
    _button.set_has_frame(false);
    _button.add_css_class("ao-playback-button");
    applySizeClass(_button, size);
    _button.set_valign(Gtk::Align::CENTER);

    _button.set_icon_name(iconForAction(action));
    _button.set_tooltip_text(labelForAction(action));

    if (showLabel)
    {
      _button.set_label(labelForAction(action));
    }

    _button.signal_clicked().connect([this] { handleClicked(); });

    auto const refreshCallback = [this] { refresh(); };
    _startedSub = _playbackService.onStarted(refreshCallback);
    _pausedSub = _playbackService.onPaused(refreshCallback);
    _idleSub = _playbackService.onIdle(refreshCallback);
    _stoppedSub = _playbackService.onStopped(refreshCallback);

    if (_action == Action::PlayPause)
    {
      _preparingSub = _playbackService.onPreparing(refreshCallback);
    }

    if (_action == Action::Shuffle)
    {
      _shuffleSub = _playbackService.onShuffleModeChanged([this](auto const&) { refresh(); });
    }

    if (_action == Action::Repeat)
    {
      _repeatSub = _playbackService.onRepeatModeChanged([this](auto const&) { refresh(); });
    }

    refresh();
  }

  void TransportButton::handleClicked()
  {
    switch (auto const& state = _playbackService.state(); _action)
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

      case Action::Next:
        if (_sequenceController != nullptr)
        {
          _sequenceController->next();
        }

        break;

      case Action::Previous:
        if (_sequenceController != nullptr)
        {
          _sequenceController->previous();
        }

        break;

      case Action::Shuffle:
        if (_sequenceController != nullptr)
        {
          _sequenceController->setShuffleMode(state.shuffleMode == rt::ShuffleMode::Off ? rt::ShuffleMode::On
                                                                                        : rt::ShuffleMode::Off);
        }

        break;

      case Action::Repeat:
        if (_sequenceController != nullptr)
        {
          if (state.repeatMode == rt::RepeatMode::Off)
          {
            _sequenceController->setRepeatMode(rt::RepeatMode::All);
          }
          else if (state.repeatMode == rt::RepeatMode::All)
          {
            _sequenceController->setRepeatMode(rt::RepeatMode::One);
          }
          else
          {
            _sequenceController->setRepeatMode(rt::RepeatMode::Off);
          }
        }

        break;
    }
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

        if (isPlaying)
        {
          _button.add_css_class("is-playing");
        }
        else
        {
          _button.remove_css_class("is-playing");
        }

        if (_showLabel)
        {
          _button.set_label(isPlaying ? "Pause" : "Play");
        }

        _button.set_sensitive(state.ready);
        break;

      case Action::Next:
      case Action::Previous:
        _button.set_sensitive(state.ready && _sequenceController != nullptr && _sequenceController->isActive());
        break;

      case Action::Shuffle:
        if (state.shuffleMode == rt::ShuffleMode::On)
        {
          _button.add_css_class("active");
        }
        else
        {
          _button.remove_css_class("active");
        }

        _button.set_sensitive(state.ready);
        break;

      case Action::Repeat:
        if (state.repeatMode == rt::RepeatMode::All)
        {
          _button.set_icon_name("media-playlist-repeat-symbolic");
          _button.add_css_class("active");
        }
        else if (state.repeatMode == rt::RepeatMode::One)
        {
          _button.set_icon_name("media-playlist-repeat-song-symbolic");
          _button.add_css_class("active");
        }
        else
        {
          _button.set_icon_name("media-playlist-repeat-symbolic");
          _button.remove_css_class("active");
        }

        _button.set_sensitive(state.ready);
        break;
    }
  }
} // namespace ao::gtk
