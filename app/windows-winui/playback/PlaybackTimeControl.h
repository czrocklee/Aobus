// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/seek/PlaybackPosition.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInteraction.h>

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
    uimodel::PlaybackTimeMode mode = uimodel::PlaybackTimeMode::Combined;
    bool presentationActive = true;
  };

  class PlaybackTimeControl final
  {
  public:
    PlaybackTimeControl(PlaybackTimeControlConfig config, rt::PlaybackService& playback);
    ~PlaybackTimeControl();

    PlaybackTimeControl(PlaybackTimeControl const&) = delete;
    PlaybackTimeControl& operator=(PlaybackTimeControl const&) = delete;
    PlaybackTimeControl(PlaybackTimeControl&&) = delete;
    PlaybackTimeControl& operator=(PlaybackTimeControl&&) = delete;

    void setPresentationActive(bool active);

  private:
    /// Establish a blank state before the first model snapshot.
    void resetPresentation();
    void stop() noexcept;

    void applyState(uimodel::PlaybackPositionViewState const& state);
    void updateRenderingRegistration();
    void stopRendering() noexcept;
    void renderFrame();
    void renderCurrentState();
    void updateText(std::chrono::milliseconds elapsed, std::chrono::milliseconds duration);

    winrt::Microsoft::UI::Xaml::Controls::TextBlock _text{nullptr};
    uimodel::PlaybackTimeMode _mode = uimodel::PlaybackTimeMode::Combined;
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
