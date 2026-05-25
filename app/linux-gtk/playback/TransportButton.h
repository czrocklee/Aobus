// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/CorePrimitives.h>
#include <ao/rt/PlaybackService.h>

#include <gtkmm/button.h>
#include <gtkmm/widget.h>

#include <cstdint>
#include <functional>
#include <string>

namespace ao::gtk
{
  class PlaybackSequenceController;

  class TransportButton final
  {
  public:
    enum class Action : std::uint8_t
    {
      Play,
      Pause,
      Stop,
      PlayPause,
      Next,
      Previous,
      Shuffle,
      Repeat,
    };

    TransportButton(rt::PlaybackService& playbackService,
                    PlaybackSequenceController* sequenceController,
                    Action action,
                    std::function<void()> onPlaySelection = {},
                    bool showLabel = false,
                    std::string const& size = "normal");
    ~TransportButton() = default;

    TransportButton(TransportButton const&) = delete;
    TransportButton& operator=(TransportButton const&) = delete;
    TransportButton(TransportButton&&) = delete;
    TransportButton& operator=(TransportButton&&) = delete;

    Gtk::Widget& widget() { return _button; }

  private:
    void refresh();
    void handleClicked();

    rt::PlaybackService& _playbackService;
    PlaybackSequenceController* _sequenceController;
    Action _action;
    std::function<void()> _onPlaySelection;
    bool _showLabel = false;
    Gtk::Button _button;

    rt::Subscription _startedSub;
    rt::Subscription _pausedSub;
    rt::Subscription _idleSub;
    rt::Subscription _stoppedSub;
    rt::Subscription _preparingSub;
    rt::Subscription _shuffleSub;
    rt::Subscription _repeatSub;
  };
} // namespace ao::gtk
