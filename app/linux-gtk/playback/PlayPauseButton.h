// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "runtime/PlaybackService.h"
#include <functional>
#include <gtkmm/button.h>
#include <string>

namespace ao::gtk::playback
{
  /**
   * @brief A composite widget providing a Play/Pause toggle button.
   */
  class PlayPauseButton final
  {
  public:
    /**
     * @brief Constructs the PlayPauseButton.
     * @param playbackService Reference to the playback service.
     * @param onPlaySelection Callback to invoke when play is clicked but no track is active.
     * @param showLabel Whether to display a text label alongside the icon.
     * @param size The size variant ("small", "normal", "large").
     */
    PlayPauseButton(ao::rt::PlaybackService& playbackService,
                    std::function<void()> onPlaySelection,
                    bool showLabel = false,
                    std::string const& size = "normal");

    /**
     * @brief Returns the underlying GTK widget.
     */
    Gtk::Widget& widget() { return _button; }

  private:
    void refresh();

    ao::rt::PlaybackService& _playbackService;
    std::function<void()> _onPlaySelection;
    Gtk::Button _button;
    bool _showLabel;

    ao::rt::Subscription _startedSub;
    ao::rt::Subscription _pausedSub;
    ao::rt::Subscription _idleSub;
    ao::rt::Subscription _stoppedSub;
    ao::rt::Subscription _preparingSub;
  };
} // namespace ao::gtk::playback
