// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/seek/PlaybackPositionInterpolator.h>
#include <ao/uimodel/playback/seek/PlaybackPositionViewModel.h>
#include <ao/uimodel/playback/seek/PlaybackTimeFormatter.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <chrono>
#include <memory>

namespace ao::winui
{
  struct WinUiDependencies;

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

    void bind(WinUiDependencies const& dependencies);
    void unbind();
    void setPresentationActive(bool active);

  private:
    void applyState(uimodel::PlaybackPositionViewState const& state);
    void updateRenderingRegistration();
    void stopRendering();
    void renderFrame();
    void renderCurrentState();
    void updateText(std::chrono::milliseconds elapsed, std::chrono::milliseconds duration);

    winrt::Microsoft::UI::Xaml::Controls::TextBlock _text{nullptr};
    uimodel::PlaybackTimeMode _mode = uimodel::PlaybackTimeMode::Default;
    uimodel::PlaybackPositionInterpolator _interpolator;
    uimodel::PlaybackPositionViewState _state{};
    std::unique_ptr<uimodel::PlaybackPositionViewModel> _viewModelPtr;
    winrt::event_token _loadedToken{};
    winrt::event_token _unloadedToken{};
    winrt::event_token _renderingToken{};
    std::chrono::seconds _lastElapsed{0};
    std::chrono::seconds _lastDuration{0};
    bool _loaded = false;
    bool _presentationActive = true;
    bool _hasState = false;
    bool _dirty = true;
    bool _rendering = false;
  };
} // namespace ao::winui
