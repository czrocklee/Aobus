// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "runtime/PlaybackService.h"
#include <gtkmm/label.h>

namespace ao::gtk::playback
{
  /**
   * @brief A composite widget displaying the current track's artist.
   */
  class NowPlayingArtistLabel final
  {
  public:
    explicit NowPlayingArtistLabel(ao::rt::PlaybackService& playbackService);

    Gtk::Widget& widget() { return _label; }

  private:
    void refresh();

    ao::rt::PlaybackService& _playbackService;
    Gtk::Label _label;

    ao::rt::Subscription _nowPlayingSub;
    ao::rt::Subscription _idleSub;
  };
} // namespace ao::gtk::playback
