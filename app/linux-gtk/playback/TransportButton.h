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

    TransportButton(ao::rt::PlaybackService& playbackService,
                    Action action,
                    std::function<void()> onPlaySelection = {},
                    bool showLabel = false,
                    std::string const& size = "normal");

    Gtk::Widget& widget() { return _button; }

  private:
    void refresh();

    ao::rt::PlaybackService& _playbackService;
    Action _action;
    std::function<void()> _onPlaySelection;
    bool _showLabel = false;
    Gtk::Button _button;

    ao::rt::Subscription _startedSub;
    ao::rt::Subscription _pausedSub;
    ao::rt::Subscription _idleSub;
    ao::rt::Subscription _stoppedSub;
    ao::rt::Subscription _preparingSub;
  };
} // namespace ao::gtk
