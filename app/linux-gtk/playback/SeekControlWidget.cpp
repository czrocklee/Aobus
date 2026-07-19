// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/SeekControlWidget.h"

#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/FrameClock.h>
#include <ao/uimodel/playback/seek/SeekSliderInteractionModel.h>
#include <ao/uimodel/playback/seek/SeekViewModel.h>

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

  SeekControlWidget::SeekControlWidget(rt::PlaybackService& playback)
    : _seekViewModel{playback, [this](ao::uimodel::SeekViewState const& view) { applyState(view); }}
  {
    _scale.set_halign(Gtk::Align::FILL);
    _scale.set_hexpand(true);
    _scale.set_valign(Gtk::Align::CENTER);
    _scale.set_draw_value(false);
    _scale.add_css_class("ao-seekbar");

    _scale.signal_value_changed().connect(sigc::mem_fun(*this, &SeekControlWidget::handleScaleValueChanged));

    // Connect user interaction gestures to debounce and prevent jumping during drag
    auto clickControllerPtr = Gtk::GestureClick::create();
    clickControllerPtr->signal_pressed().connect(
      [this](std::int32_t, double, double) { beginUserInteraction(); }, false);
    clickControllerPtr->signal_released().connect(
      [this](std::int32_t, double, double) { endUserInteraction(); }, false);
    clickControllerPtr->signal_stopped().connect(sigc::mem_fun(*this, &SeekControlWidget::endUserInteraction));
    _scale.add_controller(clickControllerPtr);

    _mapConnection = _scale.signal_map().connect(
      [this]
      {
        _isMapped = true;
        updateTickState();
      });
    _unmapConnection = _scale.signal_unmap().connect(
      [this]
      {
        stopTick();
        _isMapped = false;
      });

    reset();
  }

  SeekControlWidget::~SeekControlWidget()
  {
    stopTick();
    _debounceConnection.disconnect();
  }

  void SeekControlWidget::startTickIfNeeded()
  {
    if (!_isMapped || !_interpolator.isPlaying() || _interaction.isPointerActive() || _tickId != 0)
    {
      return;
    }

    _tickId = _scale.add_tick_callback(
      [this](Glib::RefPtr<Gdk::FrameClock> const& clockPtr) -> bool
      {
        auto const displayElapsed =
          _interpolator.interpolateElapsed(uimodel::FrameClock::fromMicros(clockPtr->get_frame_time()));
        setScaleValue(displayElapsed);

        return true;
      });
  }

  void SeekControlWidget::stopTick()
  {
    if (_tickId != 0)
    {
      _scale.remove_tick_callback(_tickId);
      _tickId = 0;
    }
  }

  void SeekControlWidget::updateTickState()
  {
    if (_isMapped && _interpolator.isPlaying() && !_interaction.isPointerActive())
    {
      startTickIfNeeded();
      return;
    }

    stopTick();
  }

  bool SeekControlWidget::isTickActive() const noexcept
  {
    return _tickId != 0;
  }

  void SeekControlWidget::applyState(ao::uimodel::SeekViewState const& view)
  {
    if (view.duration == std::chrono::milliseconds{0})
    {
      _interaction.applyViewState(view.duration, view.enabled);
      setScaleRange(std::chrono::milliseconds{0});
      setScaleValue(std::chrono::milliseconds{0});
      _scale.set_sensitive(false);
      _interpolator.reset();
      updateTickState();
      return;
    }

    setScaleRange(view.duration);
    _interaction.applyViewState(view.duration, view.enabled);
    _scale.set_sensitive(view.enabled);
    _interpolator.updateState(view.elapsed, view.duration, view.isPlaying);
    updateTickState();

    if (view.immediateUpdate && !_interaction.isPointerActive())
    {
      setScaleValue(view.elapsed);
    }
  }

  void SeekControlWidget::handleScaleValueChanged()
  {
    if (_updatingScale)
    {
      return;
    }

    applySeekDecision(_interaction.valueChanged(scaleElapsed()));
  }

  void SeekControlWidget::beginUserInteraction()
  {
    std::ignore = _interaction.beginPointerInteraction();
    updateTickState();
  }

  void SeekControlWidget::endUserInteraction()
  {
    applySeekDecision(_interaction.endPointerInteraction(scaleElapsed()));
    updateTickState();
  }

  void SeekControlWidget::applySeekDecision(uimodel::SeekSliderDecision const& decision)
  {
    switch (decision.action)
    {
      case uimodel::SeekSliderAction::Preview:
        _interpolator.updateState(decision.elapsed, _interaction.duration(), false);
        _seekViewModel.seekPreview(decision.elapsed);
        break;
      case uimodel::SeekSliderAction::Commit: commitSeekFromScale(); break;
      case uimodel::SeekSliderAction::None: break;
    }
  }

  void SeekControlWidget::executeDebouncedFinalSeek()
  {
    _debounceConnection.disconnect();

    if (_interaction.duration() > std::chrono::milliseconds{0})
    {
      _seekViewModel.seekFinal(scaleElapsed());
    }
  }

  void SeekControlWidget::commitSeekFromScale()
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

  void SeekControlWidget::setScaleRange(std::chrono::milliseconds duration)
  {
    _updatingScale = true;
    _scale.set_range(
      0.0, duration > std::chrono::milliseconds{0} ? static_cast<double>(duration.count()) : kDefaultMaxRange);
    _updatingScale = false;
  }

  void SeekControlWidget::setScaleValue(std::chrono::milliseconds elapsed)
  {
    std::chrono::milliseconds const maxDuration =
      _interaction.duration() > std::chrono::milliseconds{0} ? _interaction.duration() : std::chrono::milliseconds{0};
    auto const clampedElapsed = std::clamp(elapsed, std::chrono::milliseconds{0}, maxDuration);

    _updatingScale = true;
    _scale.set_value(static_cast<double>(clampedElapsed.count()));
    _updatingScale = false;
  }

  std::chrono::milliseconds SeekControlWidget::scaleElapsed() const noexcept
  {
    auto const upper = _interaction.duration() > std::chrono::milliseconds{0}
                         ? static_cast<double>(_interaction.duration().count())
                         : kDefaultMaxRange;
    auto const value = std::clamp(_scale.get_value(), 0.0, upper);

    return std::chrono::milliseconds{static_cast<std::int64_t>(std::round(value))};
  }

  void SeekControlWidget::reset()
  {
    _interaction.reset();

    setScaleRange(std::chrono::milliseconds{0});
    setScaleValue(std::chrono::milliseconds{0});
    _scale.set_sensitive(false);
    _interpolator.reset();
    updateTickState();
    _debounceConnection.disconnect();
  }
} // namespace ao::gtk
