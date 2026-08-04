// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/SeekControl.h"

#include "platform/ScopedBooleanFlag.h"
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/FrameClock.h>
#include <ao/uimodel/playback/seek/PlaybackPositionViewModel.h>
#include <ao/uimodel/playback/seek/SeekSliderInteractionModel.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string_view>
#include <tuple>
#include <utility>

namespace ao::winui
{
  struct SeekControl::PointerCallbackState final
  {
    explicit PointerCallbackState(SeekControl* const ownerValue)
      : owner{ownerValue}
    {
    }

    SeekControl* owner;
  };

  namespace
  {
    constexpr double kDefaultMaxRange = 100.0;
    constexpr double kIdleTrackHeight = 2.0;
    constexpr double kPointerOverTrackHeight = 6.0;
    constexpr double kModernThumbSize = 20.0;
    constexpr auto kThumbTemplateKey = std::wstring_view{L"ModernSeekThumbTemplate"};
    constexpr auto kFinalSeekDebounceInterval = std::chrono::milliseconds{50};

    winrt::Microsoft::UI::Xaml::FrameworkElement findNamedElement(
      winrt::Microsoft::UI::Xaml::DependencyObject const& root,
      std::wstring_view const name)
    {
      auto const childCount = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChildrenCount(root);

      for (std::int32_t index = 0; index < childCount; ++index)
      {
        auto const child = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChild(root, index);

        if (auto const element = child.try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>();
            element && element.Name() == name)
        {
          return element;
        }

        if (auto const descendant = findNamedElement(child, name); descendant)
        {
          return descendant;
        }
      }

      return nullptr;
    }

    uimodel::FrameClock::TimePoint currentFrameTime() noexcept
    {
      auto const microsDuration =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch());
      return uimodel::FrameClock::fromMicros(microsDuration.count());
    }
  } // namespace

  SeekControl::SeekControl(SeekControlConfig config)
    : _slider{std::move(config.slider)}
    , _thumbTemplate{std::move(config.thumbTemplate)}
    , _modernOverlay{config.modernOverlay}
    , _finalSeekTimer{_slider.DispatcherQueue().CreateTimer()}
    , _pointerCallbackStatePtr{std::make_shared<PointerCallbackState>(this)}
    , _loaded{_slider.IsLoaded()}
    , _presentationActive{config.presentationActive}
  {
    _finalSeekTimer.Interval(kFinalSeekDebounceInterval);
    _finalSeekTimer.IsRepeating(false);
    _finalSeekTickRevoker =
      _finalSeekTimer.Tick(winrt::auto_revoke,
                           [this](winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer const&,
                                  winrt::Windows::Foundation::IInspectable const&) { commitPendingFinalSeek(); });

    _valueChangedRevoker = _slider.ValueChanged(
      winrt::auto_revoke,
      [this](winrt::Windows::Foundation::IInspectable const&,
             winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args)
      {
        if (!_updating && _presentationActive && _viewModelPtr)
        {
          applySeekUpdate(_interaction.valueChanged(
            std::chrono::milliseconds{static_cast<std::int64_t>(std::round(args.NewValue()))}));
        }
      });

    auto const pointerCallbackStatePtr = std::weak_ptr{_pointerCallbackStatePtr};
    _pointerPressedHandler = winrt::box_value(winrt::Microsoft::UI::Xaml::Input::PointerEventHandler{
      [pointerCallbackStatePtr](winrt::Windows::Foundation::IInspectable const&,
                                winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
      {
        if (auto statePtr = pointerCallbackStatePtr.lock(); statePtr && statePtr->owner != nullptr)
        {
          auto* const owner = statePtr->owner;
          owner->setPointerOver(true);

          if (statePtr->owner == owner)
          {
            owner->beginPointerInteraction();
          }
        }
      }});
    _pointerReleasedHandler = winrt::box_value(winrt::Microsoft::UI::Xaml::Input::PointerEventHandler{
      [pointerCallbackStatePtr](winrt::Windows::Foundation::IInspectable const&,
                                winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
      {
        if (auto statePtr = pointerCallbackStatePtr.lock(); statePtr && statePtr->owner != nullptr)
        {
          statePtr->owner->endPointerInteraction();
        }
      }});
    _pointerCaptureLostHandler = winrt::box_value(winrt::Microsoft::UI::Xaml::Input::PointerEventHandler{
      [pointerCallbackStatePtr](winrt::Windows::Foundation::IInspectable const&,
                                winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
      {
        if (auto statePtr = pointerCallbackStatePtr.lock(); statePtr && statePtr->owner != nullptr)
        {
          statePtr->owner->endPointerInteraction();
        }
      }});
    _pointerEnteredHandler = winrt::box_value(winrt::Microsoft::UI::Xaml::Input::PointerEventHandler{
      [pointerCallbackStatePtr](winrt::Windows::Foundation::IInspectable const&,
                                winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
      {
        if (auto statePtr = pointerCallbackStatePtr.lock(); statePtr && statePtr->owner != nullptr)
        {
          statePtr->owner->setPointerOver(true);
        }
      }});
    _pointerMovedHandler = winrt::box_value(winrt::Microsoft::UI::Xaml::Input::PointerEventHandler{
      [pointerCallbackStatePtr](winrt::Windows::Foundation::IInspectable const&,
                                winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
      {
        if (auto statePtr = pointerCallbackStatePtr.lock(); statePtr && statePtr->owner != nullptr)
        {
          statePtr->owner->setPointerOver(true);
        }
      }});
    _pointerExitedHandler = winrt::box_value(winrt::Microsoft::UI::Xaml::Input::PointerEventHandler{
      [pointerCallbackStatePtr](winrt::Windows::Foundation::IInspectable const&,
                                winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
      {
        if (auto statePtr = pointerCallbackStatePtr.lock(); statePtr && statePtr->owner != nullptr)
        {
          statePtr->owner->setPointerOver(false);
        }
      }});

    _slider.AddHandler(winrt::Microsoft::UI::Xaml::UIElement::PointerPressedEvent(), _pointerPressedHandler, true);
    _slider.AddHandler(winrt::Microsoft::UI::Xaml::UIElement::PointerReleasedEvent(), _pointerReleasedHandler, true);
    _slider.AddHandler(
      winrt::Microsoft::UI::Xaml::UIElement::PointerCaptureLostEvent(), _pointerCaptureLostHandler, true);
    _slider.AddHandler(winrt::Microsoft::UI::Xaml::UIElement::PointerEnteredEvent(), _pointerEnteredHandler, true);
    _slider.AddHandler(winrt::Microsoft::UI::Xaml::UIElement::PointerMovedEvent(), _pointerMovedHandler, true);
    _slider.AddHandler(winrt::Microsoft::UI::Xaml::UIElement::PointerExitedEvent(), _pointerExitedHandler, true);

    _loadedRevoker = _slider.Loaded(
      winrt::auto_revoke,
      [this](winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
      {
        _loaded = true;
        resolveTrackElement();

        if (_presentationActive)
        {
          applyStateToSlider();
        }

        updateRenderingRegistration();
      });
    _unloadedRevoker = _slider.Unloaded(
      winrt::auto_revoke,
      [this](winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
      {
        _loaded = false;
        _pointerOver = false;
        _trackElement = nullptr;
        _decreaseTrackElement = nullptr;
        _thumbElement = nullptr;
        stopRendering();
      });

    if (_loaded)
    {
      resolveTrackElement();
    }
  }

  SeekControl::~SeekControl()
  {
    _pointerCallbackStatePtr->owner = nullptr;
    _pointerCallbackStatePtr.reset();
    unbind();

    if (_slider)
    {
      _slider.RemoveHandler(winrt::Microsoft::UI::Xaml::UIElement::PointerPressedEvent(), _pointerPressedHandler);
      _slider.RemoveHandler(winrt::Microsoft::UI::Xaml::UIElement::PointerReleasedEvent(), _pointerReleasedHandler);
      _slider.RemoveHandler(
        winrt::Microsoft::UI::Xaml::UIElement::PointerCaptureLostEvent(), _pointerCaptureLostHandler);
      _slider.RemoveHandler(winrt::Microsoft::UI::Xaml::UIElement::PointerEnteredEvent(), _pointerEnteredHandler);
      _slider.RemoveHandler(winrt::Microsoft::UI::Xaml::UIElement::PointerMovedEvent(), _pointerMovedHandler);
      _slider.RemoveHandler(winrt::Microsoft::UI::Xaml::UIElement::PointerExitedEvent(), _pointerExitedHandler);
    }
  }

  void SeekControl::bind(ao::rt::PlaybackService& playback)
  {
    unbind();
    resetPresentation();
    _viewModelPtr = std::make_unique<uimodel::PlaybackPositionViewModel>(
      playback, [this](uimodel::PlaybackPositionViewState const& state) { applyState(state); });
  }

  void SeekControl::unbind() noexcept
  {
    _viewModelPtr.reset();
    cancelPendingFinalSeek();
    stopRendering();
    _interaction.reset();
    _interpolator.reset();
    _state = {};
    _hasState = false;
  }

  void SeekControl::resetPresentation()
  {
    if (_slider && _presentationActive)
    {
      setSliderRange(std::chrono::milliseconds{0});
      setSliderValue(std::chrono::milliseconds{0});
      _slider.IsEnabled(false);
    }
  }

  void SeekControl::setPresentationActive(bool const active)
  {
    if (_presentationActive == active)
    {
      return;
    }

    _presentationActive = active;

    if (!_presentationActive)
    {
      setPointerOver(false);
      stopRendering();

      auto commitElapsed = std::chrono::milliseconds{0};
      bool shouldCommit = false;

      if (_interaction.isPointerActive())
      {
        auto const update = _interaction.endPointerInteraction(sliderElapsed());

        if (update.action == uimodel::SeekSliderAction::Commit)
        {
          commitElapsed = update.elapsed;
          shouldCommit = true;
        }
      }
      else if (_finalSeekPending)
      {
        commitElapsed = _pendingFinalElapsed;
        shouldCommit = true;
      }

      cancelPendingFinalSeek();

      if (shouldCommit && _viewModelPtr)
      {
        _viewModelPtr->seekFinal(commitElapsed);
      }

      _interaction.reset();

      if (_slider)
      {
        _slider.IsEnabled(false);
      }

      return;
    }

    if (_hasState)
    {
      _interaction.applyViewState(_state.duration, _state.seekable);
      applyStateToSlider();
    }

    updateRenderingRegistration();
  }

  void SeekControl::beginPointerInteraction()
  {
    auto const hadEarlyValueChange = _finalSeekPending;
    auto const earlyElapsed = _pendingFinalElapsed;

    if (!_presentationActive || !_viewModelPtr || !_interaction.beginPointerInteraction())
    {
      return;
    }

    cancelPendingFinalSeek();

    if (hadEarlyValueChange)
    {
      applySeekUpdate(_interaction.valueChanged(earlyElapsed));
    }

    updateRenderingRegistration();
  }

  void SeekControl::endPointerInteraction()
  {
    if (!_presentationActive || !_viewModelPtr)
    {
      return;
    }

    applySeekUpdate(_interaction.endPointerInteraction(sliderElapsed()));
    updateRenderingRegistration();
  }

  void SeekControl::applySeekUpdate(uimodel::SeekSliderUpdate const& update)
  {
    switch (update.action)
    {
      case uimodel::SeekSliderAction::Preview:
        cancelPendingFinalSeek();
        _interpolator.updateState(update.elapsed, _interaction.duration(), false);

        if (_viewModelPtr)
        {
          _viewModelPtr->seekPreview(update.elapsed);
        }

        break;
      case uimodel::SeekSliderAction::Commit: scheduleFinalSeek(update.elapsed); break;
      case uimodel::SeekSliderAction::None: break;
    }
  }

  void SeekControl::scheduleFinalSeek(std::chrono::milliseconds const elapsed)
  {
    if (!_viewModelPtr || _interaction.duration() <= std::chrono::milliseconds{0})
    {
      return;
    }

    _finalSeekTimer.Stop();
    _pendingFinalElapsed = elapsed;
    _finalSeekDeadline = std::chrono::steady_clock::now() + kFinalSeekDebounceInterval;
    _finalSeekPending = true;
    _finalSeekTimer.Interval(kFinalSeekDebounceInterval);
    _finalSeekTimer.Start();
    updateRenderingRegistration();
  }

  void SeekControl::commitPendingFinalSeek()
  {
    if (!_finalSeekPending)
    {
      return;
    }

    if (auto const now = std::chrono::steady_clock::now(); now < _finalSeekDeadline)
    {
      auto const remaining =
        std::max(std::chrono::milliseconds{1}, std::chrono::ceil<std::chrono::milliseconds>(_finalSeekDeadline - now));
      _finalSeekTimer.Stop();
      _finalSeekTimer.Interval(remaining);
      _finalSeekTimer.Start();
      return;
    }

    auto const elapsed = _pendingFinalElapsed;
    _finalSeekPending = false;

    if (_viewModelPtr)
    {
      _viewModelPtr->seekFinal(elapsed);
    }

    updateRenderingRegistration();
  }

  void SeekControl::cancelPendingFinalSeek() noexcept
  {
    _finalSeekPending = false;

    if (_finalSeekTimer)
    {
      _finalSeekTimer.Stop();
    }
  }

  void SeekControl::applyState(uimodel::PlaybackPositionViewState const& state)
  {
    _state = state;
    _hasState = true;
    _interaction.applyViewState(state.duration, state.seekable);

    if (state.duration <= std::chrono::milliseconds{0})
    {
      cancelPendingFinalSeek();
      _interpolator.reset();

      if (_presentationActive)
      {
        applyStateToSlider();
      }

      updateRenderingRegistration();
      return;
    }

    _interpolator.updateState(state.elapsed, state.duration, state.isPlaying);
    std::ignore = _interpolator.interpolateElapsed(currentFrameTime());

    if (_presentationActive)
    {
      setSliderRange(state.duration);
      _slider.IsEnabled(state.seekable);

      if (state.immediateUpdate && !_interaction.isPointerActive())
      {
        setSliderValue(state.elapsed);
      }
    }

    updateRenderingRegistration();
  }

  void SeekControl::applyStateToSlider()
  {
    if (!_slider || !_presentationActive)
    {
      return;
    }

    setSliderRange(_state.duration);
    _slider.IsEnabled(_state.seekable);

    if (_state.duration <= std::chrono::milliseconds{0})
    {
      setSliderValue(std::chrono::milliseconds{0});
      return;
    }

    setSliderValue(_interpolator.interpolateElapsed(currentFrameTime()));
  }

  void SeekControl::setSliderRange(std::chrono::milliseconds const duration)
  {
    [[maybe_unused]] auto const updating = ScopedBooleanFlag{_updating};
    _slider.Minimum(0.0);
    _slider.Maximum(duration > std::chrono::milliseconds{0} ? static_cast<double>(duration.count()) : kDefaultMaxRange);
  }

  void SeekControl::setSliderValue(std::chrono::milliseconds const elapsed)
  {
    auto const upperDuration =
      _state.duration > std::chrono::milliseconds{0} ? _state.duration : std::chrono::milliseconds{0};
    auto const clampedDuration = std::clamp(elapsed, std::chrono::milliseconds{0}, upperDuration);

    [[maybe_unused]] auto const updating = ScopedBooleanFlag{_updating};
    _slider.Value(static_cast<double>(clampedDuration.count()));
  }

  std::chrono::milliseconds SeekControl::sliderElapsed() const noexcept
  {
    auto const upperRange = _interaction.duration() > std::chrono::milliseconds{0}
                              ? static_cast<double>(_interaction.duration().count())
                              : kDefaultMaxRange;
    auto const value = std::clamp(_slider.Value(), 0.0, upperRange);
    return std::chrono::milliseconds{static_cast<std::int64_t>(std::round(value))};
  }

  void SeekControl::setPointerOver(bool const pointerOver)
  {
    if (_pointerOver == pointerOver && _trackElement && _decreaseTrackElement && _thumbElement)
    {
      return;
    }

    _pointerOver = pointerOver;

    if (!_modernOverlay)
    {
      return;
    }

    if (!_trackElement || !_decreaseTrackElement || !_thumbElement)
    {
      resolveTrackElement();
    }

    applyPointerVisual();
  }

  void SeekControl::applyPointerVisual()
  {
    auto const trackHeight = _pointerOver ? kPointerOverTrackHeight : kIdleTrackHeight;

    if (_trackElement)
    {
      _trackElement.Height(trackHeight);
    }

    if (_decreaseTrackElement)
    {
      _decreaseTrackElement.Height(trackHeight);
    }

    if (_thumbElement)
    {
      _thumbElement.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
      _thumbElement.Opacity(_pointerOver ? 1.0 : 0.0);

      if (_pointerOver)
      {
        if (auto const thumb = _thumbElement.try_as<winrt::Microsoft::UI::Xaml::Controls::Control>(); thumb)
        {
          thumb.Background(_slider.Foreground());
        }
      }
    }
  }

  void SeekControl::resolveTrackElement()
  {
    if (!_modernOverlay || !_slider || !_loaded)
    {
      return;
    }

    _slider.ApplyTemplate();
    _trackElement = findNamedElement(_slider, L"HorizontalTrackRect");
    _decreaseTrackElement = findNamedElement(_slider, L"HorizontalDecreaseRect");
    _thumbElement = findNamedElement(_slider, L"HorizontalThumb");

    if (auto const thumb = _thumbElement.try_as<winrt::Microsoft::UI::Xaml::Controls::Primitives::Thumb>(); thumb)
    {
      // Sizing the thumb is what makes the overlay hit region usable, so it is
      // applied whether or not the frame replaced the thumb's own template.
      if (auto const thumbTemplate = resolveThumbTemplate(); thumbTemplate)
      {
        thumb.Template(thumbTemplate);
      }

      thumb.MinWidth(kModernThumbSize);
      thumb.MinHeight(kModernThumbSize);
      thumb.Width(kModernThumbSize);
      thumb.Height(kModernThumbSize);
      thumb.ApplyTemplate();
    }

    applyPointerVisual();
  }

  winrt::Microsoft::UI::Xaml::Controls::ControlTemplate SeekControl::resolveThumbTemplate() const
  {
    if (_thumbTemplate)
    {
      return _thumbTemplate;
    }

    // A slider built from a document carries no resources of its own, so an
    // absent key means the frame draws the stock thumb, not that anything failed.
    auto const key = winrt::box_value(kThumbTemplateKey);
    auto const resources = _slider.Resources();

    if (!resources || !resources.HasKey(key))
    {
      return nullptr;
    }

    return resources.Lookup(key).try_as<winrt::Microsoft::UI::Xaml::Controls::ControlTemplate>();
  }

  void SeekControl::updateRenderingRegistration()
  {
    auto const shouldRender = _presentationActive && _loaded && _hasState && _interpolator.isPlaying() &&
                              !_interaction.isPointerActive() && !_finalSeekPending;

    if (shouldRender && !_rendering)
    {
      _renderingRevoker = winrt::Microsoft::UI::Xaml::Media::CompositionTarget::Rendering(
        winrt::auto_revoke,
        [this](winrt::Windows::Foundation::IInspectable const&, winrt::Windows::Foundation::IInspectable const&)
        { renderFrame(); });
      _rendering = true;
    }
    else if (!shouldRender)
    {
      stopRendering();
    }
  }

  void SeekControl::stopRendering() noexcept
  {
    if (!_rendering)
    {
      return;
    }

    _renderingRevoker.revoke();

    _rendering = false;
  }

  void SeekControl::renderFrame()
  {
    if (!_presentationActive || !_loaded || _interaction.isPointerActive() || _finalSeekPending)
    {
      updateRenderingRegistration();
      return;
    }

    setSliderValue(_interpolator.interpolateElapsed(currentFrameTime()));
  }
} // namespace ao::winui
