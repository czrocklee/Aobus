// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/PlaybackTimeControl.h"

#include "app/WinUiDependencies.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/FrameClock.h>
#include <ao/uimodel/playback/seek/PlaybackPositionViewModel.h>
#include <ao/uimodel/playback/seek/PlaybackTimeFormatter.h>

#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Foundation.h>

#include <chrono>
#include <memory>
#include <tuple>
#include <utility>

namespace ao::winui
{
  namespace
  {
    uimodel::FrameClock::TimePoint currentFrameTime() noexcept
    {
      auto const microsDuration =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch());
      return uimodel::FrameClock::fromMicros(microsDuration.count());
    }
  } // namespace

  PlaybackTimeControl::PlaybackTimeControl(PlaybackTimeControlConfig config)
    : _text{std::move(config.text)}
    , _mode{config.mode}
    , _loaded{_text.IsLoaded()}
    , _presentationActive{config.presentationActive}
  {
    _loadedToken = _text.Loaded(
      [this](winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
      {
        _loaded = true;
        renderCurrentState();
        updateRenderingRegistration();
      });
    _unloadedToken = _text.Unloaded(
      [this](winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
      {
        _loaded = false;
        stopRendering();
      });
  }

  PlaybackTimeControl::~PlaybackTimeControl()
  {
    unbind();

    if (_text)
    {
      _text.Loaded(_loadedToken);
      _text.Unloaded(_unloadedToken);
    }
  }

  void PlaybackTimeControl::bind(WinUiDependencies const& dependencies)
  {
    unbind();
    _viewModelPtr = std::make_unique<uimodel::PlaybackPositionViewModel>(
      dependencies.playbackRuntime.playback(),
      [this](uimodel::PlaybackPositionViewState const& state) { applyState(state); });
  }

  void PlaybackTimeControl::unbind()
  {
    stopRendering();
    _viewModelPtr.reset();
    _interpolator.reset();
    _state = {};
    _hasState = false;
    _dirty = true;
    _lastElapsed = std::chrono::seconds{0};
    _lastDuration = std::chrono::seconds{0};

    if (_text && _presentationActive)
    {
      _text.Text(winrt::to_hstring(uimodel::describeTimeTemplate(_mode)));
    }
  }

  void PlaybackTimeControl::setPresentationActive(bool const active)
  {
    if (_presentationActive == active)
    {
      return;
    }

    _presentationActive = active;
    _dirty = true;

    if (!_presentationActive)
    {
      stopRendering();
      return;
    }

    renderCurrentState();
    updateRenderingRegistration();
  }

  void PlaybackTimeControl::applyState(uimodel::PlaybackPositionViewState const& state)
  {
    _state = state;
    _hasState = true;

    if (state.duration <= std::chrono::milliseconds{0})
    {
      _interpolator.reset();
      _dirty = true;
      _lastElapsed = std::chrono::seconds{0};
      _lastDuration = std::chrono::seconds{0};

      if (_presentationActive)
      {
        _text.Text(winrt::to_hstring(uimodel::describeTimeTemplate(_mode)));
      }

      updateRenderingRegistration();
      return;
    }

    if (!state.isPreviewing)
    {
      _interpolator.updateState(state.elapsed, state.duration, state.isPlaying);
      std::ignore = _interpolator.interpolateElapsed(currentFrameTime());
    }

    if (_presentationActive)
    {
      updateText(state.elapsed, state.isPreviewing ? _interpolator.lastDuration() : state.duration);
    }

    updateRenderingRegistration();
  }

  void PlaybackTimeControl::updateRenderingRegistration()
  {
    auto const shouldRender =
      _presentationActive && _loaded && _hasState && !_state.isPreviewing && _interpolator.isPlaying();

    if (shouldRender && !_rendering)
    {
      _renderingToken = winrt::Microsoft::UI::Xaml::Media::CompositionTarget::Rendering(
        [this](winrt::Windows::Foundation::IInspectable const&, winrt::Windows::Foundation::IInspectable const&)
        { renderFrame(); });
      _rendering = true;
    }
    else if (!shouldRender)
    {
      stopRendering();
    }
  }

  void PlaybackTimeControl::stopRendering()
  {
    if (!_rendering)
    {
      return;
    }

    winrt::Microsoft::UI::Xaml::Media::CompositionTarget::Rendering(_renderingToken);
    _rendering = false;
  }

  void PlaybackTimeControl::renderFrame()
  {
    if (!_presentationActive || !_loaded || _state.isPreviewing)
    {
      updateRenderingRegistration();
      return;
    }

    updateText(_interpolator.interpolateElapsed(currentFrameTime()), _interpolator.lastDuration());
  }

  void PlaybackTimeControl::renderCurrentState()
  {
    if (!_presentationActive || !_loaded || !_hasState)
    {
      return;
    }

    if (_state.duration <= std::chrono::milliseconds{0})
    {
      _text.Text(winrt::to_hstring(uimodel::describeTimeTemplate(_mode)));
      return;
    }

    auto const elapsed = _state.isPreviewing ? _state.elapsed : _interpolator.interpolateElapsed(currentFrameTime());
    auto const duration = _state.isPreviewing ? _interpolator.lastDuration() : _state.duration;
    updateText(elapsed, duration);
  }

  void PlaybackTimeControl::updateText(std::chrono::milliseconds const elapsed,
                                       std::chrono::milliseconds const duration)
  {
    auto const coarseElapsed = std::chrono::duration_cast<std::chrono::seconds>(elapsed);
    auto const coarseDuration = std::chrono::duration_cast<std::chrono::seconds>(duration);

    switch (_mode)
    {
      case uimodel::PlaybackTimeMode::Elapsed:
        if (!_dirty && coarseElapsed == _lastElapsed)
        {
          return;
        }

        break;
      case uimodel::PlaybackTimeMode::Duration:
        if (!_dirty && coarseDuration == _lastDuration)
        {
          return;
        }

        break;
      case uimodel::PlaybackTimeMode::Default:
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
    _text.Text(winrt::to_hstring(uimodel::formatPlaybackTime(_mode, elapsed, duration)));
  }
} // namespace ao::winui
