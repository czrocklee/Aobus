// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "AobusSoulControl.g.h"
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>
#include <ao/uimodel/playback/transport/TransportViewModel.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include <chrono>
#include <memory>
#include <optional>

namespace ao::rt
{
  class PlaybackService;
}

namespace winrt::Aobus::implementation
{
  struct AobusSoulControl : AobusSoulControlT<AobusSoulControl>
  {
    AobusSoulControl();
    ~AobusSoulControl();

    void bind(ao::rt::PlaybackService& playback);
    void unbind();
    void setBaseStrokeWidth(double width);
    void setInnerGlyphScale(double scale);
    void setTransportIcon(ao::uimodel::TransportIcon icon);
    void setWindowActivity(bool visible, bool minimized);
    void setPresentationActive(bool active);

  private:
    void applyViewState(ao::uimodel::AobusSoulViewState const& state);
    void updateAnimationRegistration();
    void updateGeometry();
    void renderFrame();
    void stopAnimation();

    Microsoft::UI::Xaml::Controls::Grid _root{nullptr};
    Microsoft::UI::Xaml::Shapes::Ellipse _ring{nullptr};
    Microsoft::UI::Xaml::Media::LinearGradientBrush _ringBrush{nullptr};
    Microsoft::UI::Xaml::Media::SolidColorBrush _glyphBrush{nullptr};
    Microsoft::UI::Xaml::Media::GradientStop _cyanStop{nullptr};
    Microsoft::UI::Xaml::Media::GradientStop _auraStop{nullptr};
    Microsoft::UI::Xaml::Media::GradientStop _auraTailStop{nullptr};
    Microsoft::UI::Xaml::Media::RotateTransform _ringRotation{nullptr};
    Microsoft::UI::Xaml::Shapes::Polygon _playGlyph{nullptr};
    Microsoft::UI::Xaml::Controls::Grid _pauseGlyph{nullptr};

    ao::uimodel::AobusSoulViewState _viewState{};
    ao::uimodel::AobusSoulAnimationState _animation{};
    ao::uimodel::AobusSoulRgb _aura = ao::uimodel::kAobusSoulUiCyan;
    double _baseStrokeWidth = ao::uimodel::kAobusSoulGeometry.baseStrokeWidth;
    double _innerGlyphScale = 1.0;
    std::unique_ptr<ao::uimodel::AobusSoulViewModel> _viewModelPtr;
    std::optional<std::chrono::steady_clock::time_point> _optPreviousFrameTime;
    event_token _renderingToken{};
    bool _rendering = false;
    bool _loaded = false;
    bool _windowVisible = true;
    bool _windowMinimized = false;
    bool _presentationActive = true;
  };
} // namespace winrt::Aobus::implementation

namespace winrt::Aobus::factory_implementation
{
  struct AobusSoulControl : AobusSoulControlT<AobusSoulControl, implementation::AobusSoulControl>
  {};
} // namespace winrt::Aobus::factory_implementation
