// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/NowPlayingStatusLabel.h"
#include <ao/utility/Log.h>
#include <gdkmm/display.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/stylecontext.h>
#include <runtime/AppSession.h>
#include <runtime/PlaybackService.h>
#include <runtime/StateTypes.h>

#include <gdkmm/cursor.h>
#include <gtkmm/gestureclick.h>

#include <format>

namespace ao::gtk
{
  namespace
  {
    void ensureNowPlayingCss()
    {
      static auto const provider = Gtk::CssProvider::create();
      static bool initialized = false;

      if (!initialized)
      {
        provider->load_from_data(R"(
          .now-playing-label {
            transition: all 200ms ease-in-out;
            padding: 2px 12px;
            border-radius: 6px;
            color: @theme_fg_color;
            opacity: 0.85;
          }

          .now-playing-label:hover {
            background-color: alpha(@theme_selected_bg_color, 0.15);
            color: @theme_selected_bg_color;
            opacity: 1.0;
          }

          .now-playing-label:active {
            background-color: alpha(@theme_selected_bg_color, 0.25);
            opacity: 0.7;
          }

          .clickable-label {
            /* No direct cursor property in standard GTK CSS for Label */
          }
        )");

        if (auto display = Gdk::Display::get_default(); display)
        {
          Gtk::StyleContext::add_provider_for_display(display, provider, GTK_STYLE_PROVIDER_PRIORITY_USER);
        }
        initialized = true;
      }
    }
  }

  NowPlayingStatusLabel::NowPlayingStatusLabel(rt::PlaybackService& playbackService)
    : _playbackService{playbackService}
  {
    ensureNowPlayingCss();
    _label.add_css_class("now-playing-label");
    _label.add_css_class("clickable-label");
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
