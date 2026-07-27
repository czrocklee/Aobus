// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/seek/PlaybackPositionInterpolator.h>
#include <ao/uimodel/playback/seek/PlaybackPositionViewModel.h>
#include <ao/uimodel/playback/seek/SeekSliderInteractionModel.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Windows.Foundation.h>

#include <chrono>
#include <memory>

namespace ao::winui
{
  struct WinUiDependencies;

  struct SeekControlConfig final
  {
    winrt::Microsoft::UI::Xaml::Controls::Slider slider{nullptr};
    bool presentationActive = true;
    bool modernOverlay = false;
  };

  class SeekControl final
  {
  public:
    explicit SeekControl(SeekControlConfig config);
    ~SeekControl();

    SeekControl(SeekControl const&) = delete;
    SeekControl& operator=(SeekControl const&) = delete;
    SeekControl(SeekControl&&) = delete;
    SeekControl& operator=(SeekControl&&) = delete;

    void bind(WinUiDependencies const& dependencies);
    void unbind();
    void setPresentationActive(bool active);

  private:
    void beginPointerInteraction();
    void endPointerInteraction();
    void applySeekUpdate(uimodel::SeekSliderUpdate const& update);
    void scheduleFinalSeek(std::chrono::milliseconds elapsed);
    void commitPendingFinalSeek();
    void cancelPendingFinalSeek();

    void applyState(uimodel::PlaybackPositionViewState const& state);
    void applyStateToSlider();
    void setSliderRange(std::chrono::milliseconds duration);
    void setSliderValue(std::chrono::milliseconds elapsed);
    std::chrono::milliseconds sliderElapsed() const noexcept;

    void setPointerOver(bool pointerOver);
    void resolveTrackElement();
    void applyPointerVisual();

    void updateRenderingRegistration();
    void stopRendering();
    void renderFrame();

    winrt::Microsoft::UI::Xaml::Controls::Slider _slider{nullptr};
    bool _modernOverlay = false;
    winrt::Microsoft::UI::Xaml::FrameworkElement _trackElement{nullptr};
    winrt::Microsoft::UI::Xaml::FrameworkElement _decreaseTrackElement{nullptr};
    winrt::Microsoft::UI::Xaml::FrameworkElement _thumbElement{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer _finalSeekTimer{nullptr};
    winrt::Windows::Foundation::IInspectable _pointerPressedHandler{nullptr};
    winrt::Windows::Foundation::IInspectable _pointerReleasedHandler{nullptr};
    winrt::Windows::Foundation::IInspectable _pointerCaptureLostHandler{nullptr};
    winrt::Windows::Foundation::IInspectable _pointerEnteredHandler{nullptr};
    winrt::Windows::Foundation::IInspectable _pointerMovedHandler{nullptr};
    winrt::Windows::Foundation::IInspectable _pointerExitedHandler{nullptr};
    winrt::event_token _valueChangedToken{};
    winrt::event_token _loadedToken{};
    winrt::event_token _unloadedToken{};
    winrt::event_token _finalSeekTickToken{};
    winrt::event_token _renderingToken{};
    uimodel::SeekSliderInteractionModel _interaction;
    uimodel::PlaybackPositionInterpolator _interpolator;
    uimodel::PlaybackPositionViewState _state{};
    std::unique_ptr<uimodel::PlaybackPositionViewModel> _viewModelPtr;
    std::chrono::milliseconds _pendingFinalElapsed{0};
    std::chrono::steady_clock::time_point _finalSeekDeadline{};
    bool _updating = false;
    bool _loaded = false;
    bool _presentationActive = true;
    bool _hasState = false;
    bool _finalSeekPending = false;
    bool _rendering = false;
    bool _pointerOver = false;
  };
} // namespace ao::winui
