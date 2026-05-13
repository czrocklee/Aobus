// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/NowPlayingTitleLabel.h"

#include <format>

namespace ao::gtk::playback
{
  NowPlayingTitleLabel::NowPlayingTitleLabel(ao::rt::PlaybackService& playbackService)
    : _playbackService{playbackService}
  {
    _label.set_ellipsize(Pango::EllipsizeMode::END);
    _label.add_css_class("playback-title");

    auto const refreshCallback = [this] { refresh(); };
    _nowPlayingSub = _playbackService.onNowPlayingChanged([refreshCallback](auto const&) { refreshCallback(); });
    _idleSub = _playbackService.onIdle(refreshCallback);

    refresh();
  }

  void NowPlayingTitleLabel::refresh()
  {
    auto const& state = _playbackService.state();

    if (state.transport == ao::audio::Transport::Idle)
    {
      _label.set_text("Not Playing");
    }
    else
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
  }
} // namespace ao::gtk::playback
