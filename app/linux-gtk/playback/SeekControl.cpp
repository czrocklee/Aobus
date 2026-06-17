// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/SeekControl.h"

#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/FrameClock.h>
#include <ao/uimodel/playback/SeekSliderInteractionModel.h>
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
#include <tuple>

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
        if (_interpolator.isPlaying() && !_interaction.isPointerActive())
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
      _interaction.applyViewState(view.duration, view.enabled);
      setScaleRange(std::chrono::milliseconds{0});
      setScaleValue(std::chrono::milliseconds{0});
      _scale.set_sensitive(false);
      return;
    }

    setScaleRange(view.duration);
    _interaction.applyViewState(view.duration, view.enabled);
    _scale.set_sensitive(view.enabled);
    _interpolator.updateState(view.elapsed, view.duration, view.isPlaying);

    if (view.immediateUpdate && !_interaction.isPointerActive())
    {
      setScaleValue(view.elapsed);
    }
  }

  void SeekControl::handleScaleValueChanged()
  {
    if (_updatingScale)
    {
      return;
    }

    applySeekDecision(_interaction.valueChanged(scaleElapsed()));
  }

  void SeekControl::beginUserInteraction()
  {
    std::ignore = _interaction.beginPointerInteraction();
  }

  void SeekControl::endUserInteraction()
  {
    applySeekDecision(_interaction.endPointerInteraction(scaleElapsed()));
  }

  void SeekControl::applySeekDecision(uimodel::playback::SeekSliderDecision const& decision)
  {
    switch (decision.action)
    {
      case uimodel::playback::SeekSliderAction::Preview:
        _interpolator.updateState(decision.elapsed, _interaction.duration(), false);
        _controller.seekPreview(decision.elapsed);
        break;
      case uimodel::playback::SeekSliderAction::Commit: commitSeekFromScale(); break;
      case uimodel::playback::SeekSliderAction::None: break;
    }
  }

  void SeekControl::executeDebouncedFinalSeek()
  {
    _debounceConnection.disconnect();

    if (_interaction.duration() > std::chrono::milliseconds{0})
    {
      _controller.seekFinal(scaleElapsed());
    }
  }

  void SeekControl::commitSeekFromScale()
  {
    if (_interaction.duration() == std::chrono::milliseconds{0})
    {
      return;
    }

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
    _updatingScale = true;
    _scale.set_range(
      0.0, duration > std::chrono::milliseconds{0} ? static_cast<double>(duration.count()) : kDefaultMaxRange);
    _updatingScale = false;
  }

  void SeekControl::setScaleValue(std::chrono::milliseconds elapsed)
  {
    std::chrono::milliseconds const maxDuration =
      _interaction.duration() > std::chrono::milliseconds{0} ? _interaction.duration() : std::chrono::milliseconds{0};
    auto const clampedElapsed = std::clamp(elapsed, std::chrono::milliseconds{0}, maxDuration);

    _updatingScale = true;
    _scale.set_value(static_cast<double>(clampedElapsed.count()));
    _updatingScale = false;
  }

  std::chrono::milliseconds SeekControl::scaleElapsed() const noexcept
  {
    auto const upper = _interaction.duration() > std::chrono::milliseconds{0}
                         ? static_cast<double>(_interaction.duration().count())
                         : kDefaultMaxRange;
    auto const value = std::clamp(_scale.get_value(), 0.0, upper);

    return std::chrono::milliseconds{static_cast<std::int64_t>(std::round(value))};
  }

  void SeekControl::reset()
  {
    _interaction.reset();

    setScaleRange(std::chrono::milliseconds{0});
    setScaleValue(std::chrono::milliseconds{0});
    _scale.set_sensitive(false);
    _interpolator.reset();
    _debounceConnection.disconnect();
  }
} // namespace ao::gtk
