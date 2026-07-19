// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/TimeLabel.h"

#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/FrameClock.h>
#include <ao/uimodel/playback/seek/PlaybackTimeViewModel.h>

#include <gdkmm/frameclock.h>
#include <glibmm/refptr.h>
#include <gtkmm/enums.h>

#include <chrono>
#include <cstdint>

namespace ao::gtk
{
  TimeLabel::TimeLabel(rt::PlaybackService& playback, Mode mode)
    : _mode{mode}
    , _timeViewModel{playback, [this](ao::uimodel::PlaybackTimeViewState const& view) { applyState(view); }}
  {
    _label.set_halign(Gtk::Align::CENTER);
    _label.set_valign(Gtk::Align::CENTER);
    _label.add_css_class("ao-time-label");

    // Use Pango to measure the exact pixel width of the template string.
    // This ensures a tight fit without layout jitter when time changes.
    // The "tnum" feature in app.css guarantees all digits have the same width.
    auto const templateText = ao::uimodel::PlaybackTimeViewModel::describeTimeTemplate(_mode);

    auto const pangoLayoutPtr = _label.create_pango_layout(templateText);
    std::int32_t textWidth = 0;
    std::int32_t textHeight = 0;
    pangoLayoutPtr->get_pixel_size(textWidth, textHeight);

    // Lock the width to the measured size, with a small tolerance.
    _label.set_size_request(textWidth + 2, -1);

    _label.set_text(templateText);

    _mapConnection = _label.signal_map().connect(
      [this]
      {
        _isMapped = true;
        updateTickState();
      });
    _unmapConnection = _label.signal_unmap().connect(
      [this]
      {
        stopTick();
        _isMapped = false;
      });

    reset();
  }

  TimeLabel::~TimeLabel()
  {
    stopTick();
  }

  void TimeLabel::startTickIfNeeded()
  {
    if (!_isMapped || !_interpolator.isPlaying() || _isPreviewing || _tickId != 0)
    {
      return;
    }

    _tickId = _label.add_tick_callback(
      [this](Glib::RefPtr<Gdk::FrameClock> const& clockPtr) -> bool
      {
        auto const displayElapsed =
          _interpolator.interpolateElapsed(uimodel::FrameClock::fromMicros(clockPtr->get_frame_time()));
        updateLabel(displayElapsed, _interpolator.lastDuration());

        return true;
      });
  }

  void TimeLabel::stopTick()
  {
    if (_tickId != 0)
    {
      _label.remove_tick_callback(_tickId);
      _tickId = 0;
    }
  }

  void TimeLabel::updateTickState()
  {
    if (_isMapped && _interpolator.isPlaying() && !_isPreviewing)
    {
      startTickIfNeeded();
      return;
    }

    stopTick();
  }

  bool TimeLabel::isTickActive() const noexcept
  {
    return _tickId != 0;
  }

  void TimeLabel::applyState(ao::uimodel::PlaybackTimeViewState const& view)
  {
    if (view.duration == std::chrono::milliseconds{0})
    {
      reset();
      return;
    }

    _isPreviewing = view.isPreviewing;

    if (!_isPreviewing)
    {
      _interpolator.updateState(view.elapsed, view.duration, view.isPlaying);
      updateLabel(view.elapsed, view.duration);
    }
    else
    {
      updateLabel(view.elapsed, _interpolator.lastDuration());
    }

    updateTickState();
  }

  void TimeLabel::reset()
  {
    _label.set_text(ao::uimodel::PlaybackTimeViewModel::describeTimeTemplate(_mode));

    _interpolator.reset();
    _isPreviewing = false;
    updateTickState();
    _dirty = true;
    _lastElapsed = std::chrono::seconds{0};
    _lastDuration = std::chrono::seconds{0};
  }

  void TimeLabel::updateLabel(std::chrono::milliseconds elapsed, std::chrono::milliseconds duration)
  {
    auto const coarseElapsed = std::chrono::duration_cast<std::chrono::seconds>(elapsed);
    auto const coarseDuration = std::chrono::duration_cast<std::chrono::seconds>(duration);

    switch (_mode)
    {
      case Mode::Elapsed:
        if (!_dirty && coarseElapsed == _lastElapsed)
        {
          return;
        }

        break;
      case Mode::Duration:
        if (!_dirty && coarseDuration == _lastDuration)
        {
          return;
        }

        break;
      case Mode::Default:
      default:
        if (!_dirty && coarseElapsed == _lastElapsed && coarseDuration == _lastDuration)
        {
          return;
        }

        break;
    }

    _lastElapsed = coarseElapsed;
    _lastDuration = coarseDuration;
    _dirty = false;

    _label.set_text(ao::uimodel::PlaybackTimeViewModel::formatPlaybackTime(_mode, elapsed, duration));
  }
} // namespace ao::gtk
