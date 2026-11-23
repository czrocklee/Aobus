// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/SeekControl.h"

#include "platform/ScopedBooleanFlag.h"
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/FrameClock.h>
#include <ao/uimodel/playback/seek/PlaybackPosition.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInteraction.h>
#include <ao/uimodel/playback/seek/SeekStep.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <ratio>
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

    static winrt::Windows::Foundation::IInspectable makePointerEndHandler(
      std::weak_ptr<PointerCallbackState> const& pointerCallbackStatePtr,
      PointerEnd const end)
    {
      return winrt::box_value(winrt::Microsoft::UI::Xaml::Input::PointerEventHandler{
        [pointerCallbackStatePtr, end](winrt::Windows::Foundation::IInspectable const&,
                                       winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
        {
          if (auto statePtr = pointerCallbackStatePtr.lock(); statePtr && statePtr->owner != nullptr)
          {
            if (auto const pointerId = args.Pointer().PointerId(); statePtr->owner->_optPointerId == pointerId)
            {
              auto const canceled =
                end == PointerEnd::Cancel || args.GetCurrentPoint(nullptr).Properties().IsCanceled();
              statePtr->owner->endPointerInteraction(canceled ? PointerEnd::Cancel : PointerEnd::Finish);
            }
          }
        }});
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

    bool hasCaptureOnPressRoute(winrt::Microsoft::UI::Xaml::UIElement const& slider,
                                winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
      using namespace winrt::Microsoft::UI::Xaml;
      auto const pointerId = args.Pointer().PointerId();
      auto current = args.OriginalSource().try_as<DependencyObject>();

      // The native thumb or track container takes capture before the press bubbles to the slider.
      while (current)
      {
        auto const element = current.try_as<UIElement>();

        if (auto const captures = element ? element.PointerCaptures() : nullptr; captures)
        {
          for (auto const& pointer : captures)
          {
            if (pointer.PointerId() == pointerId)
            {
              return true;
            }
          }
        }

        if (current == slider)
        {
          break;
        }

        current = Media::VisualTreeHelper::GetParent(current);
      }

      return false;
    }

    uimodel::FrameClock::TimePoint currentFrameTime() noexcept
    {
      auto const microsDuration =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch());
      return uimodel::FrameClock::fromMicros(microsDuration.count());
    }
  } // namespace

  SeekControl::SeekControl(SeekControlConfig config, ao::rt::PlaybackService& playback)
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
        if (_updating || !_loaded || !_presentationActive || !_viewModelPtr)
        {
          return;
        }

        if (_interaction.isPointerActive() && !_interaction.isPointerCurrent())
        {
          // The native Slider still owns the old drag. Discard its value and
          // restore the live thumb in the same notification, until it ends.
          setSliderValue(_interpolator.interpolateElapsed(currentFrameTime()));
          return;
        }

        applySeekUpdate(
          _interaction.valueChanged(std::chrono::milliseconds{static_cast<std::int64_t>(std::round(args.NewValue()))}));
      });

    _previewKeyDownRevoker = _slider.PreviewKeyDown(
      winrt::auto_revoke,
      [this](winrt::Windows::Foundation::IInspectable const&,
             winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) { handlePreviewKeyDown(args); });

    auto const pointerCallbackStatePtr = std::weak_ptr{_pointerCallbackStatePtr};
    _pointerPressedHandler = winrt::box_value(winrt::Microsoft::UI::Xaml::Input::PointerEventHandler{
      [pointerCallbackStatePtr](winrt::Windows::Foundation::IInspectable const&,
                                winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
      {
        if (auto statePtr = pointerCallbackStatePtr.lock(); statePtr && statePtr->owner != nullptr)
        {
          auto* const owner = statePtr->owner;
          owner->setPointerOver(true);

          if (statePtr->owner == owner)
          {
            owner->beginPointerInteraction(args);
          }
        }
      }});
    _pointerEndedHandler = PointerCallbackState::makePointerEndHandler(pointerCallbackStatePtr, PointerEnd::Finish);
    _pointerCanceledHandler = PointerCallbackState::makePointerEndHandler(pointerCallbackStatePtr, PointerEnd::Cancel);
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
    _slider.AddHandler(winrt::Microsoft::UI::Xaml::UIElement::PointerReleasedEvent(), _pointerEndedHandler, true);
    _slider.AddHandler(winrt::Microsoft::UI::Xaml::UIElement::PointerCaptureLostEvent(), _pointerEndedHandler, true);
    _slider.AddHandler(winrt::Microsoft::UI::Xaml::UIElement::PointerCanceledEvent(), _pointerCanceledHandler, true);
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

        if (_optPointerId)
        {
          endPointerInteraction(PointerEnd::Cancel);
        }
      });

    if (_loaded)
    {
      resolveTrackElement();
    }

    resetPresentation();
    _viewModelPtr = std::make_unique<uimodel::PlaybackPositionViewModel>(
      playback, [this](uimodel::PlaybackPositionViewState const& state) { applyState(state); });
  }

  SeekControl::~SeekControl()
  {
    _pointerCallbackStatePtr->owner = nullptr;
    _pointerCallbackStatePtr.reset();
    stop();

    if (_slider)
    {
      _slider.RemoveHandler(winrt::Microsoft::UI::Xaml::UIElement::PointerPressedEvent(), _pointerPressedHandler);
      _slider.RemoveHandler(winrt::Microsoft::UI::Xaml::UIElement::PointerReleasedEvent(), _pointerEndedHandler);
      _slider.RemoveHandler(winrt::Microsoft::UI::Xaml::UIElement::PointerCaptureLostEvent(), _pointerEndedHandler);
      _slider.RemoveHandler(winrt::Microsoft::UI::Xaml::UIElement::PointerCanceledEvent(), _pointerCanceledHandler);
      _slider.RemoveHandler(winrt::Microsoft::UI::Xaml::UIElement::PointerEnteredEvent(), _pointerEnteredHandler);
      _slider.RemoveHandler(winrt::Microsoft::UI::Xaml::UIElement::PointerMovedEvent(), _pointerMovedHandler);
      _slider.RemoveHandler(winrt::Microsoft::UI::Xaml::UIElement::PointerExitedEvent(), _pointerExitedHandler);
    }
  }

  void SeekControl::stop() noexcept
  {
    _viewModelPtr.reset();
    cancelPendingFinalSeek();
    stopRendering();
    _optPointerId.reset();
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
      _optPointerId.reset();
      setPointerOver(false);
      stopRendering();

      auto commitUpdate = uimodel::SeekSliderUpdate{};

      if (_interaction.isPointerActive())
      {
        commitUpdate = _interaction.endPointerInteraction(sliderElapsed());
      }
      else if (_finalSeekPending)
      {
        commitUpdate = _pendingFinalUpdate;
      }

      cancelPendingFinalSeek();

      if (commitUpdate.action == uimodel::SeekSliderAction::Commit && _viewModelPtr)
      {
        _viewModelPtr->seekFinal(commitUpdate.occurrenceId, commitUpdate.elapsed);
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
      _interaction.applyViewState(_state.duration, _state.seekable, _state.occurrenceId);
      applyStateToSlider();
    }

    updateRenderingRegistration();
  }

  void SeekControl::handlePreviewKeyDown(winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args)
  {
    using namespace winrt::Microsoft::UI::Xaml;
    using winrt::Microsoft::UI::Input::InputKeyboardSource;
    using winrt::Windows::System::VirtualKey;
    using winrt::Windows::UI::Core::CoreVirtualKeyStates;

    auto const key = args.OriginalKey();

    if ((key != VirtualKey::Left && key != VirtualKey::Right) || args.Handled() || !_presentationActive ||
        !_slider.IsEnabled() || _slider.FocusState() == FocusState::Unfocused ||
        _slider.Orientation() != Controls::Orientation::Horizontal || _interaction.isPointerActive())
    {
      return;
    }

    for (auto const modifier :
         {VirtualKey::Shift, VirtualKey::Control, VirtualKey::Menu, VirtualKey::LeftWindows, VirtualKey::RightWindows})
    {
      if ((InputKeyboardSource::GetKeyStateForCurrentThread(modifier) & CoreVirtualKeyStates::Down) !=
          CoreVirtualKeyStates::None)
      {
        return;
      }
    }

    // Leave StepFrequency alone: it also sets the native pointer grid. Changing Value
    // accumulates on the displayed target and uses the same debounced final as Page/Home/End.
    auto const reversed = (_slider.FlowDirection() == FlowDirection::RightToLeft) != _slider.IsDirectionReversed();
    auto const step = std::chrono::duration<double, std::milli>{uimodel::kDefaultSeekOffset}.count();
    auto const delta = (key == VirtualKey::Right) != reversed ? step : -step;
    args.Handled(true);
    _slider.Value(std::clamp(_slider.Value() + delta, _slider.Minimum(), _slider.Maximum()));
  }

  void SeekControl::beginPointerInteraction(winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
  {
    if (!_loaded || !_presentationActive || !_viewModelPtr || _optPointerId || !hasCaptureOnPressRoute(_slider, args) ||
        !_interaction.tryBeginPointerInteraction())
    {
      return;
    }

    // Keep the first admitted pointer even if its playback occurrence is replaced.
    _optPointerId = args.Pointer().PointerId();
    auto const hadEarlyValueChange = _finalSeekPending;
    auto const earlyUpdate = _pendingFinalUpdate;
    cancelPendingFinalSeek();
    updateRenderingRegistration();

    if (hadEarlyValueChange && earlyUpdate.occurrenceId == _interaction.occurrenceId())
    {
      applySeekUpdate(_interaction.valueChanged(earlyUpdate.elapsed));
    }
  }

  void SeekControl::endPointerInteraction(PointerEnd const end)
  {
    if (!_optPointerId || !_presentationActive || !_viewModelPtr)
    {
      return;
    }

    // Release, capture loss and cancellation can arrive for the same pointer.
    _optPointerId.reset();

    auto const update = _interaction.endPointerInteraction(sliderElapsed());

    if (end == PointerEnd::Cancel)
    {
      // Drop the pending Final; only the owner of the shared preview may restore the clock.
      updateRenderingRegistration();
      _viewModelPtr->cancelPointerSeek();
      return;
    }

    applySeekUpdate(update);

    if (update.action == uimodel::SeekSliderAction::None && !_interpolator.isPlaying() && !hasPendingFinalSeek())
    {
      // Without a pending Final, a paused slider must show its live anchor.
      setSliderValue(_interpolator.interpolateElapsed(currentFrameTime()));
    }

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
          _viewModelPtr->seekPreview(update.occurrenceId, update.elapsed);
        }

        break;
      case uimodel::SeekSliderAction::Commit: scheduleFinalSeek(update); break;
      case uimodel::SeekSliderAction::None: break;
    }
  }

  void SeekControl::scheduleFinalSeek(uimodel::SeekSliderUpdate update)
  {
    if (!_viewModelPtr || update.occurrenceId.value == 0)
    {
      return;
    }

    _finalSeekTimer.Stop();
    _pendingFinalUpdate = update;
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

    auto const update = _pendingFinalUpdate;
    _finalSeekPending = false;
    _pendingFinalUpdate = {};

    if (_viewModelPtr)
    {
      _viewModelPtr->commitSliderUpdate(update);
    }

    updateRenderingRegistration();
  }

  void SeekControl::cancelPendingFinalSeek() noexcept
  {
    _finalSeekPending = false;
    _pendingFinalUpdate = {};

    if (_finalSeekTimer)
    {
      _finalSeekTimer.Stop();
    }
  }

  bool SeekControl::hasPendingFinalSeek() const noexcept
  {
    return _finalSeekPending || (_viewModelPtr && _viewModelPtr->hasPendingSliderFinal());
  }

  void SeekControl::applyState(uimodel::PlaybackPositionViewState const& state)
  {
    if (_hasState && state.occurrenceId != _state.occurrenceId)
    {
      cancelPendingFinalSeek();
    }

    _state = state;
    _hasState = true;
    auto const wasPointerCurrent = _interaction.isPointerCurrent();
    _interaction.applyViewState(state.duration, state.seekable, state.occurrenceId);
    auto const pointerBecameStale = wasPointerCurrent && !_interaction.isPointerCurrent();

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

      if ((state.immediateUpdate || pointerBecameStale) && !_interaction.isPointerCurrent() && !hasPendingFinalSeek())
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

    if (!_interaction.isPointerCurrent() && !hasPendingFinalSeek())
    {
      setSliderValue(_interpolator.interpolateElapsed(currentFrameTime()));
    }
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
                              !_interaction.isPointerCurrent() && !hasPendingFinalSeek();

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
    if (!_presentationActive || !_loaded || _interaction.isPointerCurrent() || hasPendingFinalSeek())
    {
      updateRenderingRegistration();
      return;
    }

    setSliderValue(_interpolator.interpolateElapsed(currentFrameTime()));
  }
} // namespace ao::winui
