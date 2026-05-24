// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/TimeLabel.h"

#include "ao/audio/Types.h"
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>

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
    _label.set_halign(Gtk::Align::CENTER);
    _label.set_valign(Gtk::Align::CENTER);
    _label.add_css_class("ao-time-label");

    // Use Pango to measure the exact pixel width of the template string.
    // This ensures a tight fit without layout jitter when time changes.
    // The "tnum" feature in app.css guarantees all digits have the same width.
    auto const pangoLayout = _label.create_pango_layout("00:00 / 00:00");
    std::int32_t textWidth = 0;
    std::int32_t textHeight = 0;
    pangoLayout->get_pixel_size(textWidth, textHeight);

    // Lock the width to the measured size, with a small tolerance.
    _label.set_size_request(textWidth + 2, -1);

    _label.set_text("00:00 / 00:00");

    auto const resetCallback = [this] { reset(); };

    _startedSub = _playbackService.onStarted(
      [this]
      {
        _isPreviewing = false;
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

    _seekUpdateSub = _playbackService.onSeekUpdate(
      [this](rt::PlaybackService::SeekUpdate const& ev)
      {
        bool const isPreview = ev.mode == rt::PlaybackService::SeekMode::Preview;
        _isPreviewing = isPreview;

        if (!isPreview)
        {
          auto const& state = _playbackService.state();
          bool const isPlaying =
            (state.transport == audio::Transport::Playing || state.transport == audio::Transport::Buffering ||
             state.transport == audio::Transport::Seeking);

          _interpolator.updateState(ev.positionMs, state.durationMs, isPlaying);
          updateLabel(ev.positionMs, state.durationMs);
        }
        else
        {
          updateLabel(ev.positionMs, _interpolator.lastDurationMs());
        }
      });

    _label.add_tick_callback(
      [this](Glib::RefPtr<Gdk::FrameClock> const& clock) -> bool
      {
        if (_interpolator.isPlaying() && !_isPreviewing)
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
    _isPreviewing = false;
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
