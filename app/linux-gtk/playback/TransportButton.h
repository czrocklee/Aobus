// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "runtime/PlaybackService.h"
#include <functional>
#include <gtkmm/button.h>
#include <string>

namespace ao::gtk
{
  class TransportButton final
  {
  public:
    enum class Action
    {
      Play,
      Pause,
      Stop,
      PlayPause,
    };

    TransportButton(rt::PlaybackService& playbackService,
                    Action action,
                    std::function<void()> onPlaySelection = {},
                    bool showLabel = false,
                    std::string const& size = "normal");

    Gtk::Widget& widget() { return _button; }

  private:
    void refresh();

    rt::PlaybackService& _playbackService;
    Action _action;
    std::function<void()> _onPlaySelection;
    bool _showLabel = false;
    Gtk::Button _button;

    rt::Subscription _startedSub;
    rt::Subscription _pausedSub;
    rt::Subscription _idleSub;
    rt::Subscription _stoppedSub;
    rt::Subscription _preparingSub;
  };
} // namespace ao::gtk
