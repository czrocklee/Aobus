// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/VolumeBar.h"

#include <ao/uimodel/playback/VolumeViewModel.h>

#include <cairomm/fontface.h>
#include <gdkmm/graphene_rect.h>
#include <gdkmm/rgba.h>
#include <glibmm/refptr.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/snapshot.h>
#include <gtkmm/widget.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <numbers>

namespace ao::gtk
{
  namespace
  {
    constexpr float kVolumeEpsilon = 0.001F;
    constexpr float kMinDrawHeight = 2.0F;
    constexpr float kBackgroundOpacity = 0.15F;
    constexpr float kMinHeightFactor = 0.1F;
    constexpr float kMaxHeightFactor = 0.9F;

    // Fallback accent color (Nice Blue)
    constexpr double kFallbackRed = 0.208;
    constexpr double kFallbackGreen = 0.518;
    constexpr double kFallbackBlue = 0.894;
    constexpr double kFallbackAlpha = 1.0;

    constexpr double kAngle90 = 0.5 * std::numbers::pi;
    constexpr double kAngle180 = std::numbers::pi;
    constexpr double kAngle270 = 1.5 * std::numbers::pi;
    constexpr double kAngle360 = 2.0 * std::numbers::pi;
    constexpr float kFullOpacity = 1.0F;

    // Hardware-accelerated label colors
    constexpr double kHardwareLabelRed = 0.6;
    constexpr double kHardwareLabelGreen = 0.2;
    constexpr double kHardwareLabelBlue = 0.8;

    constexpr double kHardwareLabelFontSize = 8.0;
    constexpr double kHardwareLabelYOffset = 6.0;
  }

  VolumeBar::VolumeBar()
  {
    add_css_class("ao-volume-bar");
    set_focusable(true);
    set_can_target(true);

    // Drag: Uses offsets relative to start
    auto const dragPtr = Gtk::GestureDrag::create();
    dragPtr->signal_drag_begin().connect([this](double, double) { _dragStartVolume = _volume; });
    dragPtr->signal_drag_update().connect([this](double offsetX, double) { handleDragUpdate(offsetX); });
    add_controller(dragPtr);

    // Click: Immediate jump to position
    auto const clickPtr = Gtk::GestureClick::create();
    clickPtr->signal_pressed().connect([this](std::int32_t, double offsetX, double) { handleAbsoluteClick(offsetX); });
    add_controller(clickPtr);

    // Scroll
    auto const scrollPtr = Gtk::EventControllerScroll::create();
    scrollPtr->set_flags(Gtk::EventControllerScroll::Flags::VERTICAL);
    scrollPtr->signal_scroll().connect(
      [this](double /*dx*/, double dy)
      {
        handleScroll(0.0, dy);
        return true;
      },
      false);
    add_controller(scrollPtr);
  }

  VolumeBar::~VolumeBar() = default;

  void VolumeBar::setVolume(float volume)
  {
    if (auto const clamped = std::clamp(volume, 0.0F, 1.0F); std::abs(_volume - clamped) > kVolumeEpsilon)
    {
      _volume = clamped;
      updateTooltip();
      queue_draw();
    }
  }

  float VolumeBar::volume() const
  {
    return _volume;
  }

  void VolumeBar::setIsHardwareAssisted(bool hw)
  {
    if (_isHardwareAssisted == hw)
    {
      return;
    }

    _isHardwareAssisted = hw;
    updateTooltip();
    queue_draw();
  }

  void VolumeBar::updateTooltip()
  {
    auto const percent = std::clamp(static_cast<std::int32_t>(std::round(_volume * 100.0F)), 0, 100);
    auto text = std::format("Volume: {}%", percent);

    if (_isHardwareAssisted)
    {
      text += " (Hardware)";
    }

    set_tooltip_text(text);
  }

  VolumeBar::VolumeChangedSignal& VolumeBar::signalVolumeChanged()
  {
    return _volumeChanged;
  }

  Gtk::SizeRequestMode VolumeBar::get_request_mode_vfunc() const
  {
    return Gtk::SizeRequestMode::WIDTH_FOR_HEIGHT;
  }

  void VolumeBar::measure_vfunc(Gtk::Orientation orientation,
                                int forSize,
                                int& minimum,
                                int& natural,
                                int& /*minimumBaseline*/,
                                int& /*naturalBaseline*/) const
  {
    static constexpr double kAspectRatio = std::numbers::phi + 1;
    static constexpr int kMinHeight = 24;
    static constexpr int kMinWidth = static_cast<std::int32_t>(kMinHeight * kAspectRatio);

    if (orientation == Gtk::Orientation::HORIZONTAL)
    {
      minimum = kMinWidth;
      natural = (forSize > 0)
                  ? std::max(kMinWidth, static_cast<std::int32_t>(static_cast<double>(forSize) * kAspectRatio))
                  : kMinWidth;
    }
    else
    {
      minimum = kMinHeight;
      natural = (forSize > 0)
                  ? std::max(kMinHeight, static_cast<std::int32_t>(static_cast<double>(forSize) / kAspectRatio))
                  : kMinHeight;
    }
  }

  void VolumeBar::snapshot_vfunc(Glib::RefPtr<Gtk::Snapshot> const& snapshot)
  {
    auto const width = get_width();
    auto const height = get_height();

    auto const crPtr =
      snapshot->append_cairo(Gdk::Graphene::Rect{0, 0, static_cast<float>(width), static_cast<float>(height)});

    auto const contextPtr = get_style_context();
    auto const color = contextPtr->get_color();
    auto const cssPadding = contextPtr->get_padding();

    float const vPadding = static_cast<float>(cssPadding.get_top());
    float const hPadding = static_cast<float>(cssPadding.get_left());
    float const drawHeight = std::max(kMinDrawHeight, static_cast<float>(height) - (2.0F * vPadding));
    float const yOffset = vPadding;

    float const drawWidth = std::max(0.0F, static_cast<float>(width) - (2.0F * hPadding));

    // 1. Calculate base segment width based on Soul proportion
    // Note: AobusSoul uses the full raw widget height for scaling, so we must do the same to match exactly.
    static constexpr float kSoulStrokeRatio = 9.0F / 89.124F;
    float const segmentWidth = static_cast<float>(height) * kSoulStrokeRatio;

    // 2. Determine how many segments fit
    // Use a balanced gap (e.g. 0.6x the bar width, minimum 1.5px) for the perfect density.
    float const minGap = std::max(1.5F, segmentWidth * 0.6F);

    std::int32_t numSegments = 1;
    float segmentGap = 0.0F;

    if (drawWidth > segmentWidth)
    {
      float const rawSegments = (drawWidth + minGap) / (segmentWidth + minGap);
      numSegments = std::max<std::int32_t>(1, static_cast<std::int32_t>(std::floor(rawSegments)));

      if (numSegments > 1)
      {
        segmentGap =
          (drawWidth - (static_cast<float>(numSegments) * segmentWidth)) / static_cast<float>(numSegments - 1);
      }
    }

    float const segmentRadius = segmentWidth * 0.08F;

    // Dynamically lookup the theme's accent/selection color
    auto activeColor = Gdk::RGBA{};

    if (!contextPtr->lookup_color("accent_color", activeColor))
    {
      if (!contextPtr->lookup_color("theme_selected_bg_color", activeColor))
      {
        // Fallback to a nice blue if theme doesn't provide named colors
        activeColor.set_rgba(kFallbackRed, kFallbackGreen, kFallbackBlue, kFallbackAlpha);
      }
    }

    // 1. Create the clipping path (10 rounded segments)
    // This defines the "containers" that will slice our triangle
    crPtr->save();
    crPtr->begin_new_path();

    for (std::int32_t idx = 0; idx < numSegments; ++idx)
    {
      float const segmentX = hPadding + (static_cast<float>(idx) * (segmentWidth + segmentGap));
      crPtr->begin_new_sub_path();
      crPtr->arc(segmentX + segmentRadius, yOffset + segmentRadius, segmentRadius, kAngle180, kAngle270);
      crPtr->arc(segmentX + segmentWidth - segmentRadius, yOffset + segmentRadius, segmentRadius, kAngle270, kAngle360);
      crPtr->arc(
        segmentX + segmentWidth - segmentRadius, yOffset + drawHeight - segmentRadius, segmentRadius, 0, kAngle90);
      crPtr->arc(segmentX + segmentRadius, yOffset + drawHeight - segmentRadius, segmentRadius, kAngle90, kAngle180);
      crPtr->close_path();
    }

    crPtr->clip();

    // 2. Define the "Perfect Triangle" path (a trapezoid from 10% to 100% height)
    auto const drawTrapezoid = [&](float currentWidth)
    {
      crPtr->begin_new_path();
      crPtr->move_to(0, yOffset + drawHeight);            // Bottom Left
      crPtr->line_to(currentWidth, yOffset + drawHeight); // Bottom Right
      float const hAtW =
        drawHeight * (kMinHeightFactor + kMaxHeightFactor * (currentWidth / static_cast<float>(width)));
      crPtr->line_to(currentWidth, yOffset + drawHeight - hAtW);
      crPtr->line_to(0, yOffset + drawHeight - (drawHeight * kMinHeightFactor));
      crPtr->close_path();
    };

    // 3. Draw Background (Inactive)
    drawTrapezoid(static_cast<float>(width));
    crPtr->set_source_rgba(color.get_red(), color.get_green(), color.get_blue(), kBackgroundOpacity);
    crPtr->fill();

    // 4. Draw Foreground (Active) - Clipped horizontally by volume
    if (_volume > 0.0F)
    {
      drawTrapezoid(static_cast<float>(width) * _volume);
      // Use the dynamically discovered theme color
      crPtr->set_source_rgba(activeColor.get_red(), activeColor.get_green(), activeColor.get_blue(), kFullOpacity);
      crPtr->fill();
    }

    // 5. Draw HW Indicator
    if (_isHardwareAssisted)
    {
      crPtr->set_source_rgba(kHardwareLabelRed, kHardwareLabelGreen, kHardwareLabelBlue, kFullOpacity);
      crPtr->select_font_face("sans-serif", Cairo::ToyFontFace::Slant::NORMAL, Cairo::ToyFontFace::Weight::BOLD);
      crPtr->set_font_size(kHardwareLabelFontSize);
      crPtr->move_to(0, yOffset + kHardwareLabelYOffset);
      crPtr->show_text("HW");
    }

    crPtr->restore();
  }

  void VolumeBar::handleAbsoluteClick(double offsetX)
  {
    float const vol = ao::uimodel::playback::VolumeViewModel::resolveVolumeOffset(get_width(), offsetX, 0.0F);
    setVolume(vol);
    _volumeChanged.emit(_volume);
  }

  void VolumeBar::handleDragUpdate(double offsetX)
  {
    float const vol =
      ao::uimodel::playback::VolumeViewModel::resolveVolumeOffset(get_width(), offsetX, _dragStartVolume);
    setVolume(vol);
    _volumeChanged.emit(_volume);
  }

  void VolumeBar::handleScroll(double /*dx*/, double dy)
  {
    float const newVol = ao::uimodel::playback::VolumeViewModel::resolveVolumeScroll(_volume, dy);

    if (std::abs(_volume - newVol) > kVolumeEpsilon)
    {
      _volume = newVol;
      _volumeChanged.emit(_volume);
      queue_draw();
    }
  }
} // namespace ao::gtk
