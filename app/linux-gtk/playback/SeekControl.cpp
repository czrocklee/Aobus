// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/SeekControl.h"

#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/FrameClock.h>
#include <ao/uimodel/playback/SeekViewModel.h>

#include <gdkmm/frameclock.h>
#include <glibmm/main.h>
#include <glibmm/refptr.h>
#include <gtkmm/enums.h>
#include <gtkmm/gestureclick.h>
#include <sigc++/functors/mem_fun.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace ao::gtk
{
  namespace
  {
    constexpr double kDefaultMaxRange = 100.0;
    constexpr auto kSeekDebounceInterval = std::chrono::milliseconds{50};
  } // namespace

  SeekControl::SeekControl(rt::PlaybackService& playbackService)
    : _controller{playbackService, [this](ao::uimodel::playback::SeekViewState const& view) { applyState(view); }}
  {
    _scale.set_halign(Gtk::Align::FILL);
    _scale.set_hexpand(true);
    _scale.set_valign(Gtk::Align::CENTER);
    _scale.set_draw_value(false);
    _scale.add_css_class("ao-seekbar");

    _scale.signal_value_changed().connect(sigc::mem_fun(*this, &SeekControl::handleScaleValueChanged));

    // Connect user interaction gestures to debounce and prevent jumping during drag
    auto clickControllerPtr = Gtk::GestureClick::create();
    clickControllerPtr->signal_pressed().connect(
      [this](std::int32_t, double, double) { beginUserInteraction(); }, false);
    clickControllerPtr->signal_released().connect(
      [this](std::int32_t, double, double) { endUserInteraction(); }, false);
    clickControllerPtr->signal_stopped().connect(sigc::mem_fun(*this, &SeekControl::endUserInteraction));
    _scale.add_controller(clickControllerPtr);

    _scale.add_tick_callback(
      [this](Glib::RefPtr<Gdk::FrameClock> const& clock) -> bool
      {
        if (_interpolator.isPlaying() && _interactionState == InteractionState::Idle)
        {
          auto const displayElapsed =
            _interpolator.interpolateElapsed(uimodel::FrameClock::fromMicros(clock->get_frame_time()));
          setScaleValue(displayElapsed);
        }

        return true;
      });

    reset();
  }

  SeekControl::~SeekControl()
  {
    _debounceConnection.disconnect();
  }

  void SeekControl::applyState(ao::uimodel::playback::SeekViewState const& view)
  {
    if (view.duration == std::chrono::milliseconds{0})
    {
      _interactionState = InteractionState::Idle;
      _pendingFinalSeek = false;
      setScaleRange(std::chrono::milliseconds{0});
      setScaleValue(std::chrono::milliseconds{0});
      _scale.set_sensitive(false);
      return;
    }

    setScaleRange(view.duration);
    _scale.set_sensitive(view.enabled);
    _interpolator.updateState(view.elapsed, view.duration, view.isPlaying);

    if (view.immediateUpdate && _interactionState == InteractionState::Idle)
    {
      setScaleValue(view.elapsed);
    }
  }

  void SeekControl::handleScaleValueChanged()
  {
    if (_updatingScale || _duration == std::chrono::milliseconds{0})
    {
      return;
    }

    if (_interactionState == InteractionState::Pointer)
    {
      previewSeekFromScale();
      return;
    }

    commitSeekFromScale();
  }

  void SeekControl::beginUserInteraction()
  {
    if (!_scale.get_sensitive() || _duration == std::chrono::milliseconds{0})
    {
      return;
    }

    if (_interactionState != InteractionState::Pointer)
    {
      _pendingFinalSeek = false;
    }

    _interactionState = InteractionState::Pointer;
  }

  void SeekControl::endUserInteraction()
  {
    if (_interactionState != InteractionState::Pointer)
    {
      return;
    }

    _interactionState = InteractionState::Idle;

    if (_pendingFinalSeek)
    {
      commitSeekFromScale();
    }
  }

  void SeekControl::previewSeekFromScale()
  {
    auto const elapsed = scaleElapsed();
    _pendingFinalSeek = true;
    _interpolator.updateState(elapsed, _duration, false);
    _controller.seekPreview(elapsed);
  }

  void SeekControl::executeDebouncedFinalSeek()
  {
    _debounceConnection.disconnect();

    if (_duration > std::chrono::milliseconds{0})
    {
      _controller.seekFinal(scaleElapsed());
    }
  }

  void SeekControl::commitSeekFromScale()
  {
    if (_duration == std::chrono::milliseconds{0})
    {
      return;
    }

    _pendingFinalSeek = false;
    _debounceConnection.disconnect();

    _debounceConnection = Glib::signal_timeout().connect(
      [this] -> bool
      {
        executeDebouncedFinalSeek();
        return false;
      },
      kSeekDebounceInterval.count());
  }

  void SeekControl::setScaleRange(std::chrono::milliseconds duration)
  {
    _duration = duration;

    _updatingScale = true;
    _scale.set_range(
      0.0, duration > std::chrono::milliseconds{0} ? static_cast<double>(duration.count()) : kDefaultMaxRange);
    _updatingScale = false;
  }

  void SeekControl::setScaleValue(std::chrono::milliseconds elapsed)
  {
    std::chrono::milliseconds const maxDuration =
      _duration > std::chrono::milliseconds{0} ? _duration : std::chrono::milliseconds{0};
    auto const clampedElapsed = std::clamp(elapsed, std::chrono::milliseconds{0}, maxDuration);

    _updatingScale = true;
    _scale.set_value(static_cast<double>(clampedElapsed.count()));
    _updatingScale = false;
  }

  std::chrono::milliseconds SeekControl::scaleElapsed() const noexcept
  {
    auto const upper =
      _duration > std::chrono::milliseconds{0} ? static_cast<double>(_duration.count()) : kDefaultMaxRange;
    auto const value = std::clamp(_scale.get_value(), 0.0, upper);

    return std::chrono::milliseconds{static_cast<std::int64_t>(std::round(value))};
  }

  void SeekControl::reset()
  {
    _interactionState = InteractionState::Idle;
    _pendingFinalSeek = false;

    setScaleRange(std::chrono::milliseconds{0});
    setScaleValue(std::chrono::milliseconds{0});
    _scale.set_sensitive(false);
    _interpolator.reset();
    _debounceConnection.disconnect();
  }
} // namespace ao::gtk
