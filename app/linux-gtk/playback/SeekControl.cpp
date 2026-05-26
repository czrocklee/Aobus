// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/SeekControl.h"

#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/playback/PlaybackPositionInterpolator.h>
#include <ao/uimodel/playback/SeekViewModel.h>

#include <gdkmm/frameclock.h>
#include <glibmm/main.h>
#include <gtkmm/enums.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturedrag.h>

#include <algorithm>
#include <cstdint>
#include <memory>

namespace ao::gtk
{
  namespace
  {
    constexpr double kDefaultMaxRange = 100.0;
    constexpr int kSeekDebounceMs = 50;
  }

  SeekControl::SeekControl(rt::PlaybackService& playbackService)
  {
    _scale.set_range(0, kDefaultMaxRange);
    _scale.set_value(0);
    _scale.set_sensitive(false);
    _scale.set_hexpand(true);
    _scale.set_valign(Gtk::Align::CENTER);
    _scale.add_css_class("ao-seekbar");

    _scale.signal_value_changed().connect([this] { handleScaleValueChanged(); });

    auto const clickGesture = Gtk::GestureClick::create();
    clickGesture->set_button(1);
    clickGesture->signal_pressed().connect([this](std::int32_t, double, double) { beginUserInteraction(); });
    clickGesture->signal_released().connect([this](std::int32_t, double, double) { endUserInteraction(); });
    _scale.add_controller(clickGesture);

    auto const dragGesture = Gtk::GestureDrag::create();
    dragGesture->set_button(1);
    dragGesture->signal_drag_begin().connect([this](double, double) { beginUserInteraction(); });
    dragGesture->signal_drag_end().connect([this](double, double) { endUserInteraction(); });
    _scale.add_controller(dragGesture);

    _controller = std::make_unique<ao::uimodel::playback::SeekViewModel>(
      playbackService, [this](ao::uimodel::playback::SeekViewState const& view) { applyState(view); });

    _scale.add_tick_callback(
      [this](Glib::RefPtr<Gdk::FrameClock> const& clock) -> bool
      {
        if (_interpolator.isPlaying() && _interactionState == InteractionState::Idle)
        {
          auto const displayPos = _interpolator.interpolate(clock->get_frame_time());
          setScaleValue(displayPos);
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
    if (view.durationMs == 0)
    {
      _interactionState = InteractionState::Idle;
      _pendingFinalSeek = false;
      setScaleRange(0);
      setScaleValue(0);
      _scale.set_sensitive(false);
      return;
    }

    setScaleRange(view.durationMs);
    _scale.set_sensitive(view.enabled);
    _interpolator.updateState(view.positionMs, view.durationMs, view.isPlaying);

    if (view.immediateUpdate && _interactionState == InteractionState::Idle)
    {
      setScaleValue(view.positionMs);
    }
  }

  void SeekControl::handleScaleValueChanged()
  {
    if (_updatingScale || _durationMs == 0)
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
    if (!_scale.get_sensitive() || _durationMs == 0)
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
    auto const positionMs = scalePositionMs();
    _pendingFinalSeek = true;
    _interpolator.updateState(positionMs, _durationMs, false);
    _controller->seekPreview(positionMs);
  }

  void SeekControl::executeDebouncedFinalSeek()
  {
    _debounceConnection.disconnect();

    if (_durationMs > 0)
    {
      _controller->seekFinal(scalePositionMs());
    }
  }

  void SeekControl::commitSeekFromScale()
  {
    if (_durationMs == 0)
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
      kSeekDebounceMs);
  }

  void SeekControl::setScaleRange(std::uint32_t const durationMs)
  {
    _durationMs = durationMs;

    _updatingScale = true;
    _scale.set_range(0, durationMs > 0 ? static_cast<double>(durationMs) : kDefaultMaxRange);
    _updatingScale = false;
  }

  void SeekControl::setScaleValue(std::uint32_t const positionMs)
  {
    std::uint32_t const upper = _durationMs > 0 ? _durationMs : 0;
    auto const clampedPositionMs = std::min(positionMs, upper);

    _updatingScale = true;
    _scale.set_value(static_cast<double>(clampedPositionMs));
    _updatingScale = false;
  }

  std::uint32_t SeekControl::scalePositionMs() const noexcept
  {
    auto const upper = _durationMs > 0 ? static_cast<double>(_durationMs) : kDefaultMaxRange;
    auto const value = std::clamp(_scale.get_value(), 0.0, upper);

    return static_cast<std::uint32_t>(value);
  }

  void SeekControl::reset()
  {
    _interactionState = InteractionState::Idle;
    _pendingFinalSeek = false;

    setScaleRange(0);
    setScaleValue(0);
    _scale.set_sensitive(false);
    _interpolator.reset();
    _debounceConnection.disconnect();
  }
} // namespace ao::gtk
