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

    NowPlayingFieldLabel(rt::PlaybackService& playbackService, Field field);

    Gtk::Widget& widget() { return _label; }

  private:
    void refresh();

    rt::PlaybackService& _playbackService;
    Field _field;
    Gtk::Label _label;

    rt::Subscription _nowPlayingSub;
    rt::Subscription _idleSub;
  };
} // namespace ao::gtk
