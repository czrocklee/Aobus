// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "playback/AobusSoul.h"
#include <gtkmm/window.h>

namespace ao::gtk
{
  /**
   * @class AobusSoulWindow
   * @brief A fullscreen overlay that displays a giant Aobus Soul.
   */
  class AobusSoulWindow final : public Gtk::Window
  {
  public:
    AobusSoulWindow();
    ~AobusSoulWindow() override;

    /**
     * @brief Set the quality and state for the big soul.
     */
    void updateState(ao::audio::Quality quality, bool isPlaying);

  protected:
    // Close on any key or button press - handled via Gtk::ShortcutController

  private:
    void ensureCss();

    AobusSoul _bigSoul;
    std::uint32_t _tickId = 0;
    double _animationTime = 0.0;
    std::int64_t _firstFrameTime = 0;

    ao::audio::Quality _currentQuality = ao::audio::Quality::Unknown;
    bool _isPlaying = false;

    static constexpr int kDefaultLogoHeight = 400;
  };
} // namespace ao::gtk
