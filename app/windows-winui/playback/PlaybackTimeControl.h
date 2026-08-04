// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/seek/PlaybackPositionInterpolator.h>
#include <ao/uimodel/playback/seek/PlaybackPositionViewModel.h>
#include <ao/uimodel/playback/seek/PlaybackTimeFormatter.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>

#include <chrono>
#include <memory>

namespace ao::rt
{
  class PlaybackService;
} // namespace ao::rt

namespace ao::winui
{
  struct PlaybackTimeControlConfig final
  {
    winrt::Microsoft::UI::Xaml::Controls::TextBlock text{nullptr};
    uimodel::PlaybackTimeMode mode = uimodel::PlaybackTimeMode::Default;
    bool presentationActive = true;
  };

  class PlaybackTimeControl final
  {
  public:
    explicit PlaybackTimeControl(PlaybackTimeControlConfig config);
    ~PlaybackTimeControl();

    PlaybackTimeControl(PlaybackTimeControl const&) = delete;
    PlaybackTimeControl& operator=(PlaybackTimeControl const&) = delete;
    PlaybackTimeControl(PlaybackTimeControl&&) = delete;
    PlaybackTimeControl& operator=(PlaybackTimeControl&&) = delete;

    void bind(rt::PlaybackService& playback);
    void unbind() noexcept;
    void setPresentationActive(bool active);

  private:
    /// Blank the widget between bindings. Only a rebind has anything to show.
    void resetPresentation();

    void applyState(uimodel::PlaybackPositionViewState const& state);
    void updateRenderingRegistration();
    void stopRendering() noexcept;
    void renderFrame();
    void renderCurrentState();
    void updateText(std::chrono::milliseconds elapsed, std::chrono::milliseconds duration);

    winrt::Microsoft::UI::Xaml::Controls::TextBlock _text{nullptr};
    uimodel::PlaybackTimeMode _mode = uimodel::PlaybackTimeMode::Default;
    uimodel::PlaybackPositionInterpolator _interpolator;
    uimodel::PlaybackPositionViewState _state{};
    std::unique_ptr<uimodel::PlaybackPositionViewModel> _viewModelPtr;
    winrt::Microsoft::UI::Xaml::Controls::TextBlock::Loaded_revoker _loadedRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock::Unloaded_revoker _unloadedRevoker{};
    winrt::Microsoft::UI::Xaml::Media::CompositionTarget::Rendering_revoker _renderingRevoker{};
    std::chrono::seconds _lastElapsed{0};
    std::chrono::seconds _lastDuration{0};
    bool _loaded = false;
    bool _presentationActive = true;
    bool _hasState = false;
    bool _dirty = true;
    bool _rendering = false;
  };
} // namespace ao::winui
