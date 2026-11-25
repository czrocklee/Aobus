// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/SeekControlWidget.h"

#include "common/AccessibleLabel.h"
#include "i18n/GtkText.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/FrameClock.h>
#include <ao/uimodel/playback/seek/PlaybackPosition.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInteraction.h>
#include <ao/uimodel/playback/seek/SeekStep.h>

#include <gdk/gdkkeysyms.h>
#include <gdkmm/enums.h>
#include <gdkmm/event.h>
#include <gdkmm/frameclock.h>
#include <gdkmm/surface.h>
#include <glibmm/main.h>
#include <glibmm/refptr.h>
#include <gtkmm/accelerator.h>
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

  SeekControlWidget::SeekControlWidget(rt::PlaybackService& playback, i18n::MessageCatalog const& textCatalog)
    : _seekViewModel{playback, [this](ao::uimodel::PlaybackPositionViewState const& view) { applyState(view); }}
  {
    _scale.set_halign(Gtk::Align::FILL);
    _scale.set_hexpand(true);
    _scale.set_valign(Gtk::Align::CENTER);
    _scale.set_draw_value(false);
    _scale.add_css_class("ao-seekbar");

    // The scale has no visible label of its own; give assistive technology a name
    // and a hover tooltip consistent with the volume control.
    setTooltipAndAccessibleLabel(_scale, gtkText(textCatalog, i18n::MessageId::PlaybackPosition));

    // GtkRange still tracks the physical drag after its occurrence is replaced. Veto its
    // native changes before they move the thumb; live position updates can still set it.
    _changeValueConnection = _scale.signal_change_value().connect(
      [this](Gtk::ScrollType, double) { return _interaction.isPointerActive() && !_interaction.isPointerCurrent(); },
      false);
    _valueChangedConnection =
      _scale.signal_value_changed().connect(sigc::mem_fun(*this, &SeekControlWidget::handleScaleValueChanged));

    // GtkRange claims its own drag and cancels competing GestureClick observers.
    // Observe physical input without participating in that gesture arbitration.
    auto inputControllerPtr = Gtk::EventControllerLegacy::create();
    inputControllerPtr->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    _inputEventConnection = inputControllerPtr->signal_event().connect(
      [this](Glib::RefPtr<Gdk::Event const> const& eventPtr)
      {
        if (eventPtr->get_event_type() == Gdk::Event::Type::KEY_PRESS)
        {
          return tryHandleSeekKey(*eventPtr);
        }

        handlePointerEvent(eventPtr);
        return false;
      },
      false);
    _scale.add_controller(inputControllerPtr);
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
    _inputEventConnection.disconnect();
    _changeValueConnection.disconnect();
    _valueChangedConnection.disconnect();
    _stateFlagsConnection.disconnect();
    _mapConnection.disconnect();
    _unmapConnection.disconnect();
    _pointerPressEventPtr.reset();
    stopTick();
    _debounceConnection.disconnect();
  }

  bool SeekControlWidget::hasPendingFinalSeek() const noexcept
  {
    return _optPendingFinalSeek || _seekViewModel.hasPendingSliderFinal();
  }

  void SeekControlWidget::startTickIfNeeded()
  {
    if (_tickId != 0)
    {
      return;
    }

    _tickId = _scale.add_tick_callback(
      [this](Glib::RefPtr<Gdk::FrameClock> const& clockPtr) -> bool
      {
        if (_interaction.isPointerCurrent() || hasPendingFinalSeek())
        {
          _tickId = 0;
          return false;
        }

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
    if (_isMapped && _interpolator.isPlaying() && !_interaction.isPointerCurrent() && !hasPendingFinalSeek())
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

    if (view.immediateUpdate && !_interaction.isPointerCurrent() && !hasPendingFinalSeek())
    {
      setScaleValue(view.elapsed);
    }
  }

  bool SeekControlWidget::tryHandleSeekKey(Gdk::Event const& event)
  {
    if (!_scale.has_focus() || !_scale.is_sensitive() || _scale.get_orientation() != Gtk::Orientation::HORIZONTAL ||
        _interaction.isPointerActive() ||
        (event.get_modifier_state() & Gtk::Accelerator::get_default_mod_mask()) != Gdk::ModifierType{})
    {
      return false;
    }

    auto const key = event.get_keyval();
    bool const isRight = key == GDK_KEY_Right || key == GDK_KEY_KP_Right;

    if (!isRight && key != GDK_KEY_Left && key != GDK_KEY_KP_Left)
    {
      return false;
    }

    bool const flipsForRtl = _scale.get_flippable() && _scale.get_direction() == Gtk::TextDirection::RTL;
    bool const isInverted = _scale.get_inverted() != flipsForRtl;
    constexpr auto kSeekOffset = std::chrono::duration_cast<std::chrono::milliseconds>(uimodel::kDefaultSeekOffset);
    auto const delta = isRight != isInverted ? kSeekOffset : -kSeekOffset;

    // Leave the adjustment increments alone so other inputs retain their native behavior.
    // GtkRange clamps the displayed value and value_changed debounces the final seek.
    _scale.set_value(_scale.get_value() + static_cast<double>(delta.count()));
    return true;
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

    if (wasPointerActive && update.action == uimodel::SeekSliderAction::None && !_interpolator.isPlaying() &&
        !hasPendingFinalSeek())
    {
      // Release without a seek returns to the live position, even without a frame tick.
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
      _seekViewModel.commitSliderUpdate(*optUpdate);
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
