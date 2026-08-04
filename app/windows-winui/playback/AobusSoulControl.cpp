// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/AobusSoulControl.h"

#include "pch.h"

#if __has_include("AobusSoulControl.g.cpp")
#include "AobusSoulControl.g.cpp"
#endif

#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>
#include <ao/uimodel/playback/transport/TransportViewModel.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>

namespace winrt::Aobus::implementation
{
  namespace
  {
    constexpr double kGeometryEpsilon = 0.001;
    constexpr float kGeometryMidpoint = 0.5F;
    constexpr double kPauseBarCornerRadius = 1.5;
    constexpr double kPauseGlyphWidthScale = 0.8;
    constexpr double kPauseColumnSpacingScale = 0.22;

    Windows::UI::Color color(ao::uimodel::AobusSoulRgb const value, std::uint8_t const alpha = 0xFF) noexcept
    {
      return Windows::UI::Color{.A = alpha, .R = value.red, .G = value.green, .B = value.blue};
    }
  } // namespace

  AobusSoulControl::AobusSoulControl()
  {
    HorizontalContentAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
    VerticalContentAlignment(Microsoft::UI::Xaml::VerticalAlignment::Stretch);
    IsTabStop(false);

    _root = Microsoft::UI::Xaml::Controls::Grid{};
    _ring = Microsoft::UI::Xaml::Shapes::Ellipse{};
    _ring.Stretch(Microsoft::UI::Xaml::Media::Stretch::Fill);
    _ring.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Center);
    _ring.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
    _ringRotation = Microsoft::UI::Xaml::Media::RotateTransform{};
    _ring.RenderTransform(_ringRotation);
    _ring.RenderTransformOrigin({kGeometryMidpoint, kGeometryMidpoint});

    _ringBrush = Microsoft::UI::Xaml::Media::LinearGradientBrush{};
    _ringBrush.ColorInterpolationMode(Microsoft::UI::Xaml::Media::ColorInterpolationMode::SRgbLinearInterpolation);
    _ringBrush.StartPoint({1.0F, 1.0F});
    _ringBrush.EndPoint({0.0F, 0.0F});
    _cyanStop = Microsoft::UI::Xaml::Media::GradientStop{};
    _cyanStop.Offset(0.0);
    _auraStop = Microsoft::UI::Xaml::Media::GradientStop{};
    _auraStop.Offset(ao::uimodel::kAobusSoulCoreGradientStop);
    _auraTailStop = Microsoft::UI::Xaml::Media::GradientStop{};
    _auraTailStop.Offset(1.0);
    _ringBrush.GradientStops().Append(_cyanStop);
    _ringBrush.GradientStops().Append(_auraStop);
    _ringBrush.GradientStops().Append(_auraTailStop);
    _ring.Stroke(_ringBrush);
    _root.Children().Append(_ring);

    _glyphBrush = Microsoft::UI::Xaml::Media::SolidColorBrush{color(ao::uimodel::kAobusSoulUiCyan)};
    _playGlyph = Microsoft::UI::Xaml::Shapes::Polygon{};
    _playGlyph.Points().Append({0.0F, 0.0F});
    _playGlyph.Points().Append({1.0F, kGeometryMidpoint});
    _playGlyph.Points().Append({0.0F, 1.0F});
    _playGlyph.Fill(_glyphBrush);
    _playGlyph.Stretch(Microsoft::UI::Xaml::Media::Stretch::Fill);
    _playGlyph.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Center);
    _playGlyph.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
    _root.Children().Append(_playGlyph);

    _pauseGlyph = Microsoft::UI::Xaml::Controls::Grid{};
    _pauseGlyph.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Center);
    _pauseGlyph.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
    _pauseGlyph.ColumnDefinitions().Append(Microsoft::UI::Xaml::Controls::ColumnDefinition{});
    _pauseGlyph.ColumnDefinitions().Append(Microsoft::UI::Xaml::Controls::ColumnDefinition{});
    auto leftBar = Microsoft::UI::Xaml::Shapes::Rectangle{};
    leftBar.Fill(_glyphBrush);
    leftBar.RadiusX(kPauseBarCornerRadius);
    leftBar.RadiusY(kPauseBarCornerRadius);
    auto rightBar = Microsoft::UI::Xaml::Shapes::Rectangle{};
    rightBar.Fill(_glyphBrush);
    rightBar.RadiusX(kPauseBarCornerRadius);
    rightBar.RadiusY(kPauseBarCornerRadius);
    Microsoft::UI::Xaml::Controls::Grid::SetColumn(rightBar, 1);
    _pauseGlyph.Children().Append(leftBar);
    _pauseGlyph.Children().Append(rightBar);
    _root.Children().Append(_pauseGlyph);
    Content(_root);

    Loaded(
      [this](Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
      {
        _loaded = true;
        updateGeometry();
        updateAnimationRegistration();
      });
    Unloaded(
      [this](Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
      {
        _loaded = false;
        stopAnimation();
      });
    SizeChanged([this](Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&)
                { updateGeometry(); });

    applyViewState({});
    setTransportIcon(ao::uimodel::TransportIcon::None);
  }

  AobusSoulControl::~AobusSoulControl()
  {
    unbind();
    stopAnimation();
  }

  void AobusSoulControl::bind(ao::rt::PlaybackService& playback)
  {
    unbind();
    resetPresentation();
    _viewModelPtr = std::make_unique<ao::uimodel::AobusSoulViewModel>(
      playback, [this](ao::uimodel::AobusSoulViewState const& state) { applyViewState(state); });
  }

  void AobusSoulControl::unbind() noexcept
  {
    _viewModelPtr.reset();
    stopAnimation();
  }

  void AobusSoulControl::resetPresentation()
  {
    applyViewState({});
  }

  void AobusSoulControl::setBaseStrokeWidth(double const width)
  {
    if (width <= 0.0 || std::abs(_baseStrokeWidth - width) < kGeometryEpsilon)
    {
      return;
    }

    _baseStrokeWidth = width;
    updateGeometry();
  }

  void AobusSoulControl::setInnerGlyphScale(double const scale)
  {
    if (scale <= 0.0 || std::abs(_innerGlyphScale - scale) < kGeometryEpsilon)
    {
      return;
    }

    _innerGlyphScale = scale;
    updateGeometry();
  }

  void AobusSoulControl::setTransportIcon(ao::uimodel::TransportIcon const icon)
  {
    _playGlyph.Visibility(icon == ao::uimodel::TransportIcon::Play ? Microsoft::UI::Xaml::Visibility::Visible
                                                                   : Microsoft::UI::Xaml::Visibility::Collapsed);
    _pauseGlyph.Visibility(icon == ao::uimodel::TransportIcon::Pause ? Microsoft::UI::Xaml::Visibility::Visible
                                                                     : Microsoft::UI::Xaml::Visibility::Collapsed);
  }

  void AobusSoulControl::setWindowActivity(bool const visible, bool const minimized)
  {
    _windowVisible = visible;
    _windowMinimized = minimized;
    updateAnimationRegistration();
  }

  void AobusSoulControl::setPresentationActive(bool const active)
  {
    _presentationActive = active;
    updateAnimationRegistration();
  }

  void AobusSoulControl::applyViewState(ao::uimodel::AobusSoulViewState const& state)
  {
    _viewState = state;
    _animation.setMotionMode(state.motionMode);
    _aura = ao::uimodel::aobusSoulAuraRgb(state.aura);
    updateAnimationRegistration();
    renderFrame();
  }

  void AobusSoulControl::updateAnimationRegistration()
  {
    auto const animate = ao::uimodel::shouldAnimateAobusSoul(
      _viewState.motionMode, _loaded && _windowVisible && _presentationActive, _windowMinimized);

    if (animate && !_rendering)
    {
      _optPreviousFrameTime.reset();
      _renderingRevoker = Microsoft::UI::Xaml::Media::CompositionTarget::Rendering(
        winrt::auto_revoke,
        [this](Windows::Foundation::IInspectable const&, Windows::Foundation::IInspectable const&) { renderFrame(); });
      _rendering = true;
    }
    else if (!animate)
    {
      stopAnimation();
    }
  }

  void AobusSoulControl::stopAnimation() noexcept
  {
    _renderingRevoker.revoke();
    _rendering = false;

    _optPreviousFrameTime.reset();
  }

  void AobusSoulControl::updateGeometry()
  {
    auto const available = std::min(ActualWidth(), ActualHeight());

    if (available <= 0.0)
    {
      return;
    }

    auto const& geometry = ao::uimodel::kAobusSoulGeometry;
    auto const expandedStroke = _baseStrokeWidth * ao::uimodel::kAobusSoulGoldenRatio;
    auto const scale = available / ((geometry.radius + (expandedStroke / 2.0)) * 2.0);
    auto const diameter = geometry.radius * 2.0 * scale;
    auto const glyphDiameter = geometry.innerGlyphRadius * 1.55 * scale * _innerGlyphScale;
    _ring.Width(diameter);
    _ring.Height(diameter);
    _playGlyph.Width(glyphDiameter);
    _playGlyph.Height(glyphDiameter);
    _pauseGlyph.Width(glyphDiameter * kPauseGlyphWidthScale);
    _pauseGlyph.Height(glyphDiameter);
    _pauseGlyph.ColumnSpacing(glyphDiameter * kPauseColumnSpacingScale);
    renderFrame();
  }

  void AobusSoulControl::renderFrame()
  {
    if (_rendering && _animation.motionMode() == ao::uimodel::AobusSoulMotionMode::Animating)
    {
      auto const frameTime = std::chrono::steady_clock::now();

      if (_optPreviousFrameTime)
      {
        _animation.advance(frameTime - *_optPreviousFrameTime);
      }

      _optPreviousFrameTime = frameTime;
    }

    auto const visual = _animation.visualFrame(_aura);
    _cyanStop.Color(color(visual.gradientColors.core));
    _auraStop.Color(color(visual.gradientColors.body));
    _auraTailStop.Color(color(visual.gradientColors.body));
    _glyphBrush.Color(color(visual.gradientColors.body));
    _ringRotation.Angle(visual.motion.rotationDegrees);
    _ring.Opacity(visual.motion.luminance);
    _playGlyph.Opacity(visual.motion.luminance);
    _pauseGlyph.Opacity(visual.motion.luminance);

    auto const available = std::min(ActualWidth(), ActualHeight());

    if (available > 0.0)
    {
      auto const& geometry = ao::uimodel::kAobusSoulGeometry;
      auto const expandedStroke = _baseStrokeWidth * ao::uimodel::kAobusSoulGoldenRatio;
      auto const scale = available / ((geometry.radius + (expandedStroke / 2.0)) * 2.0);
      auto const stroke = _baseStrokeWidth + ((expandedStroke - _baseStrokeWidth) * visual.motion.breath);
      _ring.StrokeThickness(stroke * scale);
    }
  }
} // namespace winrt::Aobus::implementation
