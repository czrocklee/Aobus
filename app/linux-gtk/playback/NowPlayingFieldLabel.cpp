// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/NowPlayingFieldLabel.h"

#include "ao/audio/Types.h"
#include "ao/utility/Log.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/WorkspaceService.h>

#include <gdkmm/cursor.h>
#include <gtkmm/eventcontroller.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/label.h>
#include <pangomm/layout.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace ao::gtk
{
  namespace
  {
    char const* cssClassForField(rt::TrackField field)
    {
      switch (field)
      {
        case rt::TrackField::Title: return "ao-playback-title";
        case rt::TrackField::Artist: return "ao-playback-artist";
        default: return "";
      }
    }

    std::string quoteExpressionString(std::string_view value)
    {
      if (!value.contains('"'))
      {
        return std::format("\"{}\"", value);
      }

      if (!value.contains('\''))
      {
        return std::format("'{}'", value);
      }

      auto sanitized = std::string{value};
      std::ranges::replace(sanitized, '"', '\'');
      return std::format("\"{}\"", sanitized);
    }
  } // namespace

  NowPlayingFieldLabel::NowPlayingFieldLabel(rt::AppRuntime& runtime, rt::TrackField field, Action action)
    : _runtime{runtime}, _field{field}, _action{action}
  {
    _label.set_ellipsize(Pango::EllipsizeMode::END);
    _label.add_css_class(cssClassForField(field));
    _label.set_selectable(false); // Selection can block click gestures

    if (_action != Action::None)
    {
      _label.add_css_class("ao-clickable");
      _label.set_cursor(Gdk::Cursor::create("pointer"));

      auto const gesture = Gtk::GestureClick::create();
      gesture->set_button(1);
      gesture->set_propagation_phase(Gtk::PropagationPhase::BUBBLE);
      gesture->signal_pressed().connect([this](std::int32_t, double, double) { onLabelClicked(); });
      _label.add_controller(gesture);
    }

    auto const refreshCallback = [this] { refresh(); };
    _nowPlayingSub = _runtime.playback().onNowPlayingChanged([refreshCallback](auto const&) { refreshCallback(); });
    _idleSub = _runtime.playback().onIdle(refreshCallback);

    refresh();
  }

  void NowPlayingFieldLabel::onLabelClicked()
  {
    APP_LOG_DEBUG("[PID {}] NowPlayingFieldLabel: Clicked, field: {}, action: {}",
                  getpid(),
                  static_cast<int>(_field),
                  static_cast<int>(_action));

    switch (_action)
    {
      case Action::Reveal: _runtime.playback().revealPlayingTrack(); break;

      case Action::PlayPause:
        if (_runtime.playback().state().transport == audio::Transport::Playing)
        {
          _runtime.playback().pause();
        }
        else
        {
          _runtime.playback().resume();
        }

        break;

      case Action::FilterByField:
      {
        auto const& state = _runtime.playback().state();
        auto value = std::string{};

        switch (_field)
        {
          case rt::TrackField::Title: value = state.trackTitle; break;
          case rt::TrackField::Artist: value = state.trackArtist; break;
          default: break;
        }

        if (auto const variable = rt::trackFieldFilterExpressionVariable(_field); !variable.empty() && !value.empty())
        {
          auto const query = std::format("{} = {}", variable, quoteExpressionString(value));
          APP_LOG_DEBUG("[PID {}] NowPlayingFieldLabel: Navigating to query: {}", getpid(), query);
          _runtime.workspace().navigateTo(query);
        }
        else
        {
          APP_LOG_DEBUG("[PID {}] NowPlayingFieldLabel: Navigation skipped (empty variable or value)", getpid());
        }

        break;
      }

      case Action::None:
      default: break;
    }
  }

  void NowPlayingFieldLabel::refresh()
  {
    auto const& state = _runtime.playback().state();

    if (state.transport == audio::Transport::Idle)
    {
      if (_field == rt::TrackField::Title)
      {
        _label.set_text("Not Playing");
      }
      else
      {
        _label.set_text("");
      }

      return;
    }

    if (_field == rt::TrackField::Title)
    {
      if (!state.trackTitle.empty())
      {
        _label.set_text(state.trackTitle);
      }
      else
      {
        _label.set_text(std::format("{}", state.trackId.raw()));
      }
    }
    else if (_field == rt::TrackField::Artist)
    {
      if (!state.trackArtist.empty())
      {
        _label.set_text(state.trackArtist);
      }
      else
      {
        _label.set_text("Unknown Artist");
      }
    }
    else
    {
      _label.set_text("");
    }
  }
} // namespace ao::gtk
