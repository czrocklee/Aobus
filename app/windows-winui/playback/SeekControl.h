// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/seek/PlaybackPosition.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInteraction.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Foundation.h>

#include <chrono>
#include <memory>

namespace ao::rt
{
  class PlaybackService;
} // namespace ao::rt

namespace ao::winui
{
  struct SeekControlConfig final
  {
    winrt::Microsoft::UI::Xaml::Controls::Slider slider{nullptr};
    bool presentationActive = true;
    bool modernOverlay = false;

    /**
     * @brief The thumb the overlay presentation draws, when the frame supplies one.
     *
     * The overlay thumb is a `ControlTemplate` and therefore cannot come from a
     * `Style`, so it is handed in like any other frame-owned template. Absent,
     * the slider keeps the stock thumb rather than failing.
     */
    winrt::Microsoft::UI::Xaml::Controls::ControlTemplate thumbTemplate{nullptr};
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

    void bind(rt::PlaybackService& playback);
    void unbind() noexcept;
    void setPresentationActive(bool active);

  private:
    struct PointerCallbackState;

    /// Blank the widget between bindings. Only a rebind has anything to show.
    void resetPresentation();

    void beginPointerInteraction();
    void endPointerInteraction();
    void applySeekUpdate(uimodel::SeekSliderUpdate const& update);
    void scheduleFinalSeek(std::chrono::milliseconds elapsed);
    void commitPendingFinalSeek();
    void cancelPendingFinalSeek() noexcept;

    void applyState(uimodel::PlaybackPositionViewState const& state);
    void applyStateToSlider();
    void setSliderRange(std::chrono::milliseconds duration);
    void setSliderValue(std::chrono::milliseconds elapsed);
    std::chrono::milliseconds sliderElapsed() const noexcept;

    void setPointerOver(bool pointerOver);
    void resolveTrackElement();
    winrt::Microsoft::UI::Xaml::Controls::ControlTemplate resolveThumbTemplate() const;
    void applyPointerVisual();

    void updateRenderingRegistration();
    void stopRendering() noexcept;
    void renderFrame();

    winrt::Microsoft::UI::Xaml::Controls::Slider _slider{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ControlTemplate _thumbTemplate{nullptr};
    bool _modernOverlay = false;
    winrt::Microsoft::UI::Xaml::FrameworkElement _trackElement{nullptr};
    winrt::Microsoft::UI::Xaml::FrameworkElement _decreaseTrackElement{nullptr};
    winrt::Microsoft::UI::Xaml::FrameworkElement _thumbElement{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer _finalSeekTimer{nullptr};
    std::shared_ptr<PointerCallbackState> _pointerCallbackStatePtr;
    winrt::Windows::Foundation::IInspectable _pointerPressedHandler{nullptr};
    winrt::Windows::Foundation::IInspectable _pointerReleasedHandler{nullptr};
    winrt::Windows::Foundation::IInspectable _pointerCaptureLostHandler{nullptr};
    winrt::Windows::Foundation::IInspectable _pointerEnteredHandler{nullptr};
    winrt::Windows::Foundation::IInspectable _pointerMovedHandler{nullptr};
    winrt::Windows::Foundation::IInspectable _pointerExitedHandler{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Slider::ValueChanged_revoker _valueChangedRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::Slider::Loaded_revoker _loadedRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::Slider::Unloaded_revoker _unloadedRevoker{};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer::Tick_revoker _finalSeekTickRevoker{};
    winrt::Microsoft::UI::Xaml::Media::CompositionTarget::Rendering_revoker _renderingRevoker{};
    uimodel::SeekInteraction _interaction;
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
