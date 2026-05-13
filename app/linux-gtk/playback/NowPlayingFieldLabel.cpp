// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/NowPlayingFieldLabel.h"

#include <format>

namespace ao::gtk
{
  namespace
  {
    char const* cssClassForField(NowPlayingFieldLabel::Field field)
    {
      switch (field)
      {
        case NowPlayingFieldLabel::Field::Title: return "playback-title";
        case NowPlayingFieldLabel::Field::Artist: return "playback-artist";
      }
      return "";
    }
  } // namespace

  NowPlayingFieldLabel::NowPlayingFieldLabel(rt::PlaybackService& playbackService, Field field)
    : _playbackService{playbackService}, _field{field}
  {
    _label.set_ellipsize(Pango::EllipsizeMode::END);
    _label.add_css_class(cssClassForField(field));

    auto const refreshCallback = [this] { refresh(); };
    _nowPlayingSub = _playbackService.onNowPlayingChanged([refreshCallback](auto const&) { refreshCallback(); });
    _idleSub = _playbackService.onIdle(refreshCallback);

    refresh();
  }

  void NowPlayingFieldLabel::refresh()
  {
    auto const& state = _playbackService.state();

    if (state.transport == audio::Transport::Idle)
    {
      if (_field == Field::Title)
      {
        _label.set_text("Not Playing");
      }
      else
      {
        _label.set_text("");
      }
      return;
    }

    if (_field == Field::Title)
    {
      if (!state.trackTitle.empty())
      {
        _label.set_text(state.trackTitle);
      }
      else
      {
        _label.set_text(std::format("{}", state.trackId.value()));
      }
    }
    else
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
  }
} // namespace ao::gtk
