// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/SeekControl.h"
#include <runtime/PlaybackService.h>
#include <runtime/StateTypes.h>

#include <gdkmm/frameclock.h>
#include <glibmm/refptr.h>
#include <gtkmm/enums.h>
#include <gtkmm/gestureclick.h>

#include <cstdint>

namespace ao::gtk
{
  namespace
  {
    constexpr double kDefaultMaxRange = 100.0;
  }

  SeekControl::SeekControl(rt::PlaybackService& playbackService)
    : _playbackService{playbackService}
  {
    _scale.set_range(0, kDefaultMaxRange);
    _scale.set_value(0);
    _scale.set_sensitive(false);
    _scale.set_hexpand(true);
    _scale.set_valign(Gtk::Align::CENTER);

    auto const gesture = Gtk::GestureClick::create();
    gesture->set_button(1);

    gesture->signal_pressed().connect(
      [this](int /*n_press*/, double posX, double /*posY*/)
      {
        _isDragging = true;
        int const width = _scale.get_width();

        if (width > 0)
        {
          double const newValue = (posX / static_cast<double>(width)) * static_cast<double>(_durationMs);
          _scale.set_value(newValue);
          _playbackService.seek(static_cast<std::uint32_t>(newValue));
        }
      });

    gesture->signal_released().connect(
      [this](int, double, double)
      {
        _isDragging = false;
        _playbackService.seek(static_cast<std::uint32_t>(_scale.get_value()));
      });

    _scale.add_controller(gesture);

    auto const resetCallback = [this] { reset(); };

    _startedSub = _playbackService.onStarted(
      [this]
      {
        auto const& state = _playbackService.state();
        _interpolator.updateState(state.positionMs, state.durationMs, true);

        if (state.durationMs > 0)
        {
          _durationMs = state.durationMs;
          _updating = true;
          _scale.set_range(0, static_cast<double>(state.durationMs));
          _scale.set_value(static_cast<double>(state.positionMs));
          _updating = false;
          _scale.set_sensitive(true);
        }
        else
        {
          _durationMs = 0;
          _scale.set_range(0, kDefaultMaxRange);
          _scale.set_sensitive(false);
        }
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

    _scale.add_tick_callback(
      [this](Glib::RefPtr<Gdk::FrameClock> const& clock) -> bool
      {
        if (_interpolator.isPlaying() && !_isDragging)
        {
          auto const displayPos = _interpolator.interpolate(clock->get_frame_time());

          _updating = true;
          _scale.set_value(static_cast<double>(displayPos));
          _updating = false;
        }

        return true;
      });

    reset();
  }

  void SeekControl::reset()
  {
    _scale.set_value(0);

    _durationMs = 0;
    _scale.set_range(0, kDefaultMaxRange);
    _scale.set_sensitive(false);
    _interpolator.reset();
  }
} // namespace ao::gtk
