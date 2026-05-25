// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/SeekControl.h"

#include "ao/audio/Types.h"
#include <ao/rt/PlaybackService.h>

#include <gdkmm/frameclock.h>
#include <glibmm/main.h>
#include <glibmm/refptr.h>
#include <gtkmm/enums.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturedrag.h>

#include <algorithm>
#include <cstdint>

namespace ao::gtk
{
  namespace
  {
    constexpr double kDefaultMaxRange = 100.0;
    constexpr int kSeekDebounceMs = 50;

    bool isAdvancingTransport(audio::Transport const transport) noexcept
    {
      return transport == audio::Transport::Playing || transport == audio::Transport::Buffering ||
             transport == audio::Transport::Seeking;
    }
  }

  SeekControl::SeekControl(rt::PlaybackService& playbackService)
    : _playbackService{playbackService}
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
    clickGesture->signal_pressed().connect([this](std::int32_t /*nPress*/, double /*posX*/, double /*posY*/)
                                           { beginUserInteraction(); });
    clickGesture->signal_released().connect([this](std::int32_t /*nPress*/, double /*posX*/, double /*posY*/)
                                            { endUserInteraction(); });
    _scale.add_controller(clickGesture);

    auto const dragGesture = Gtk::GestureDrag::create();
    dragGesture->set_button(1);
    dragGesture->signal_drag_begin().connect([this](double /*posX*/, double /*posY*/) { beginUserInteraction(); });
    dragGesture->signal_drag_end().connect([this](double /*offsetX*/, double /*offsetY*/) { endUserInteraction(); });
    _scale.add_controller(dragGesture);

    auto const resetCallback = [this] { reset(); };

    _startedSub = _playbackService.onStarted([this] { handleStarted(); });

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
        if (ev.mode == rt::PlaybackService::SeekMode::Final)
        {
          auto const& state = _playbackService.state();

          setScaleRange(state.durationMs);
          _scale.set_sensitive(state.durationMs > 0);
          _interpolator.updateState(ev.positionMs, state.durationMs, isAdvancingTransport(state.transport));

          if (_interactionState == InteractionState::Idle)
          {
            setScaleValue(ev.positionMs);
          }
        }
        else if (ev.mode == rt::PlaybackService::SeekMode::Preview)
        {
          _interpolator.updateState(ev.positionMs, _durationMs, _interpolator.isPlaying());
        }
      });

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

  void SeekControl::handleStarted()
  {
    _interactionState = InteractionState::Idle;
    _pendingFinalSeek = false;

    auto const& state = _playbackService.state();
    _interpolator.updateState(state.positionMs, state.durationMs, true);
    setScaleRange(state.durationMs);

    if (state.durationMs > 0)
    {
      setScaleValue(state.positionMs);
      _scale.set_sensitive(true);
    }
    else
    {
      setScaleValue(0);
      _scale.set_sensitive(false);
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
    _playbackService.seek(positionMs, rt::PlaybackService::SeekMode::Preview);
  }

  void SeekControl::executeDebouncedFinalSeek()
  {
    _debounceConnection.disconnect();

    if (_durationMs > 0)
    {
      _playbackService.seek(scalePositionMs(), rt::PlaybackService::SeekMode::Final);
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
