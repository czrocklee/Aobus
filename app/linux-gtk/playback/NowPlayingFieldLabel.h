// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "runtime/PlaybackService.h"
#include <gtkmm/label.h>

namespace ao::gtk
{
  class NowPlayingFieldLabel final
  {
  public:
    enum class Field
    {
      Title,
      Artist,
    };

    NowPlayingFieldLabel(ao::rt::PlaybackService& playbackService, Field field);

    Gtk::Widget& widget() { return _label; }

  private:
    void refresh();

    ao::rt::PlaybackService& _playbackService;
    Field _field;
    Gtk::Label _label;

    ao::rt::Subscription _nowPlayingSub;
    ao::rt::Subscription _idleSub;
  };
} // namespace ao::gtk
