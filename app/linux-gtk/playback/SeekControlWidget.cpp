// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/SeekControlWidget.h"

#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/FrameClock.h>
#include <ao/uimodel/playback/seek/PlaybackPosition.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInteraction.h>

#include <gdkmm/event.h>
#include <gdkmm/frameclock.h>
#include <gdkmm/surface.h>
#include <glibmm/main.h>
#include <glibmm/refptr.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontroller.h>
#include <gtkmm/eventcontrollerlegacy.h>
#include <sigc++/functors/mem_fun.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    constexpr double kDefaultMaxRange = 100.0;
    constexpr auto kSeekDebounceInterval = std::chrono::milliseconds{50};
  } // namespace

  SeekControlWidget::SeekControlWidget(rt::PlaybackService& playback)
    : _seekViewModel{playback, [this](ao::uimodel::PlaybackPositionViewState const& view) { applyState(view); }}
  {
    _scale.set_halign(Gtk::Align::FILL);
    _scale.set_hexpand(true);
    _scale.set_valign(Gtk::Align::CENTER);
    _scale.set_draw_value(false);
    _scale.add_css_class("ao-seekbar");

    _valueChangedConnection =
      _scale.signal_value_changed().connect(sigc::mem_fun(*this, &SeekControlWidget::handleScaleValueChanged));

    // GtkRange claims its own drag and cancels competing GestureClick observers.
    // Observe physical input without participating in that gesture arbitration.
    auto pointerControllerPtr = Gtk::EventControllerLegacy::create();
    pointerControllerPtr->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    _pointerEventConnection = pointerControllerPtr->signal_event().connect(
      [this](Glib::RefPtr<Gdk::Event const> const& eventPtr)
      {
        handlePointerEvent(eventPtr);
        return false;
      },
      false);
    _scale.add_controller(pointerControllerPtr);
    _stateFlagsConnection = _scale.signal_state_flags_changed().connect(
      [this](Gtk::StateFlags)
      {
        if (!_scale.is_sensitive())
        {
          endUserInteraction();
        }
      });

    _mapConnection = _scale.signal_map().connect(
      [this]
      {
        _isMapped = true;
        updateTickState();
      });
    _unmapConnection = _scale.signal_unmap().connect(
      [this]
      {
        _isMapped = false;
        stopTick();
        endUserInteraction();
      });

    // Nothing to install here: the view model already delivered the current
    // playback state synchronously during member construction, and its
    // zero-duration branch installs the disabled, zeroed scale itself.
  }

  SeekControlWidget::~SeekControlWidget()
  {
    _pointerEventConnection.disconnect();
    _valueChangedConnection.disconnect();
    _stateFlagsConnection.disconnect();
    _mapConnection.disconnect();
    _unmapConnection.disconnect();
    _pointerPressEventPtr.reset();
    stopTick();
    _debounceConnection.disconnect();
  }

  void SeekControlWidget::startTickIfNeeded()
  {
    if (!_isMapped || !_interpolator.isPlaying() || _interaction.isPointerActive() || _optPendingFinalSeek ||
        _tickId != 0)
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
    if (_isMapped && _interpolator.isPlaying() && !_interaction.isPointerActive() && !_optPendingFinalSeek)
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

  void SeekControlWidget::applyState(ao::uimodel::PlaybackPositionViewState const& view)
  {
    if (view.occurrenceId != _presentedOccurrenceId)
    {
      _debounceConnection.disconnect();
      _optPendingFinalSeek.reset();
      _presentedOccurrenceId = view.occurrenceId;
    }

    if (view.duration <= std::chrono::milliseconds{0})
    {
      _debounceConnection.disconnect();
      _optPendingFinalSeek.reset();
      _interaction.applyViewState(view.duration, view.seekable, view.occurrenceId);
      setScaleRange(std::chrono::milliseconds{0});
      setScaleValue(std::chrono::milliseconds{0});
      _scale.set_sensitive(false);
      _interpolator.reset();
      updateTickState();
      return;
    }

    setScaleRange(view.duration);
    _interaction.applyViewState(view.duration, view.seekable, view.occurrenceId);
    _scale.set_sensitive(view.seekable);
    _interpolator.updateState(view.elapsed, view.duration, view.isPlaying);
    updateTickState();

    if (view.immediateUpdate && !_interaction.isPointerActive() && !_optPendingFinalSeek)
    {
      setScaleValue(view.elapsed);
    }
  }

  void SeekControlWidget::handlePointerEvent(Glib::RefPtr<Gdk::Event const> const& eventPtr)
  {
    auto const matchesPointer = _pointerPressEventPtr &&
                                eventPtr->get_device() == _pointerPressEventPtr->get_device() &&
                                eventPtr->get_event_sequence() == _pointerPressEventPtr->get_event_sequence();

    switch (eventPtr->get_event_type())
    {
      case Gdk::Event::Type::BUTTON_PRESS:
      case Gdk::Event::Type::TOUCH_BEGIN: beginUserInteraction(eventPtr); break;
      case Gdk::Event::Type::BUTTON_RELEASE:
        if (matchesPointer && _pointerPressEventPtr->get_event_type() == Gdk::Event::Type::BUTTON_PRESS &&
            eventPtr->get_button() == _pointerPressEventPtr->get_button())
        {
          endUserInteraction();
        }

        break;
      case Gdk::Event::Type::TOUCH_END:
      case Gdk::Event::Type::TOUCH_CANCEL:
        if (matchesPointer && _pointerPressEventPtr->get_event_type() == Gdk::Event::Type::TOUCH_BEGIN)
        {
          endUserInteraction();
        }

        break;
      case Gdk::Event::Type::GRAB_BROKEN:
        if (_pointerPressEventPtr && eventPtr->get_device() == _pointerPressEventPtr->get_device() &&
            eventPtr->get_grab_broken_grab_surface() != _pointerPressEventPtr->get_surface())
        {
          endUserInteraction();
        }

        break;
      default: break;
    }
  }

  void SeekControlWidget::handleScaleValueChanged()
  {
    if (_updatingScale)
    {
      return;
    }

    applySeekUpdate(_interaction.valueChanged(scaleElapsed()));
  }

  void SeekControlWidget::beginUserInteraction(Glib::RefPtr<Gdk::Event const> const& eventPtr)
  {
    auto const optEarlyUpdate = _optPendingFinalSeek;

    if (_pointerPressEventPtr || !_interaction.tryBeginPointerInteraction())
    {
      return;
    }

    _pointerPressEventPtr = eventPtr;
    _debounceConnection.disconnect();
    _optPendingFinalSeek.reset();

    if (optEarlyUpdate && optEarlyUpdate->occurrenceId == _interaction.occurrenceId())
    {
      applySeekUpdate(_interaction.valueChanged(optEarlyUpdate->elapsed));
    }

    updateTickState();
  }

  void SeekControlWidget::endUserInteraction()
  {
    auto const wasPointerActive = _interaction.isPointerActive();
    _pointerPressEventPtr.reset();
    auto const update = _interaction.endPointerInteraction(scaleElapsed());
    applySeekUpdate(update);

    if (wasPointerActive && update.action == uimodel::SeekSliderAction::None && !_interpolator.isPlaying())
    {
      // A paused replacement has no frame tick to retire the old drag's value.
      setScaleValue(_interpolator.interpolateElapsed(uimodel::FrameClock::TimePoint{}));
    }

    updateTickState();
  }

  void SeekControlWidget::applySeekUpdate(uimodel::SeekSliderUpdate const& update)
  {
    switch (update.action)
    {
      case uimodel::SeekSliderAction::Preview:
        _interpolator.updateState(update.elapsed, _interaction.duration(), false);
        _seekViewModel.seekPreview(update.occurrenceId, update.elapsed);
        break;
      case uimodel::SeekSliderAction::Commit: scheduleFinalSeek(update); break;
      case uimodel::SeekSliderAction::None: break;
    }
  }

  void SeekControlWidget::executeDebouncedFinalSeek()
  {
    _debounceConnection.disconnect();
    auto const optUpdate = std::exchange(_optPendingFinalSeek, std::nullopt);

    if (optUpdate)
    {
      _seekViewModel.seekFinal(optUpdate->occurrenceId, optUpdate->elapsed);
    }

    updateTickState();
  }

  void SeekControlWidget::scheduleFinalSeek(uimodel::SeekSliderUpdate update)
  {
    if (update.occurrenceId.value == 0)
    {
      return;
    }

    _debounceConnection.disconnect();
    _optPendingFinalSeek = update;

    _debounceConnection = Glib::signal_timeout().connect(
      [this] -> bool
      {
        executeDebouncedFinalSeek();
        return false;
      },
      kSeekDebounceInterval.count());
    updateTickState();
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
} // namespace ao::gtk
