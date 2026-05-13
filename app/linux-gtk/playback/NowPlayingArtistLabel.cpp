// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/NowPlayingArtistLabel.h"

namespace ao::gtk::playback
{
  NowPlayingArtistLabel::NowPlayingArtistLabel(ao::rt::PlaybackService& playbackService)
    : _playbackService{playbackService}
  {
    _label.set_ellipsize(Pango::EllipsizeMode::END);
    _label.add_css_class("playback-artist");

    auto const refreshCallback = [this] { refresh(); };
    _nowPlayingSub = _playbackService.onNowPlayingChanged([refreshCallback](auto const&) { refreshCallback(); });
    _idleSub = _playbackService.onIdle(refreshCallback);

    refresh();
  }

  void NowPlayingArtistLabel::refresh()
  {
    auto const& state = _playbackService.state();

    if (state.transport == ao::audio::Transport::Idle)
    {
      _label.set_text("");
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
} // namespace ao::gtk::playback
