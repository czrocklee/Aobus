// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/TimeLabel.h"
#include <runtime/PlaybackService.h>
#include <runtime/StateTypes.h>

#include <gdkmm/frameclock.h>
#include <glibmm/refptr.h>
#include <gtkmm/enums.h>

#include <cstdint>
#include <format>

namespace ao::gtk
{
  TimeLabel::TimeLabel(rt::PlaybackService& playbackService)
    : _playbackService{playbackService}
  {
    _label.set_halign(Gtk::Align::END);
    _label.set_valign(Gtk::Align::CENTER);

    int const preferredWidthChars = 15;
    _label.set_width_chars(preferredWidthChars);
    _label.set_text("00:00 / 00:00");

    auto const resetCallback = [this] { reset(); };

    _startedSub = _playbackService.onStarted(
      [this]
      {
        auto const& state = _playbackService.state();
        _interpolator.updateState(state.positionMs, state.durationMs, true);
        updateLabel(state.positionMs, state.durationMs);
      });

    _pausedSub = _playbackService.onPaused(
      [this]
      {
        auto const& state = _playbackService.state();
        _interpolator.updateState(state.positionMs, state.durationMs, false);
      });

    _idleSub = _playbackService.onIdle(resetCallback);
    _stoppedSub = _playbackService.onStopped(resetCallback);
    _preparingSub = _playbackService.onPreparing(resetCallback);

    _label.add_tick_callback(
      [this](Glib::RefPtr<Gdk::FrameClock> const& clock) -> bool
      {
        if (_interpolator.isPlaying())
        {
          auto const displayPos = _interpolator.interpolate(clock->get_frame_time());
          updateLabel(displayPos, _interpolator.lastDurationMs());
        }

        return true;
      });

    reset();
  }

  void TimeLabel::reset()
  {
    _label.set_text("00:00 / 00:00");
    _interpolator.reset();
  }

  void TimeLabel::updateLabel(std::uint32_t posMs, std::uint32_t durMs)
  {
    int const msInSec = 1000;
    auto const posSec = posMs / msInSec;
    auto const durSec = durMs / msInSec;

    int const secInMin = 60;
    _label.set_text(std::format(
      "{:d}:{:02d} / {:d}:{:02d}", posSec / secInMin, posSec % secInMin, durSec / secInMin, durSec % secInMin));
  }
} // namespace ao::gtk
