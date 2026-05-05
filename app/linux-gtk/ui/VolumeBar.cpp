// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "VolumeBar.h"
#include <algorithm>
#include <cmath>

namespace ao::gtk
{
  namespace
  {
    constexpr float kVolumeEpsilon = 0.001F;
    constexpr int kMinHorizontalWidth = 32;
    constexpr int kNaturalHorizontalWidth = 42;
    constexpr int kMinVerticalHeight = 20;
    constexpr int kNaturalVerticalHeight = 26;
    constexpr float kVerticalPadding = 4.0F;
    constexpr float kMinDrawHeight = 2.0F;
    constexpr float kBackgroundOpacity = 0.15F;
    constexpr float kMinHeightFactor = 0.1F;
    constexpr float kMaxHeightFactor = 0.9F;

    // Fallback accent color (Nice Blue)
    constexpr double kFallbackRed = 0.208;
    constexpr double kFallbackGreen = 0.518;
    constexpr double kFallbackBlue = 0.894;
    constexpr double kFallbackAlpha = 1.0;

    constexpr double kAngle90 = 0.5 * M_PI;
    constexpr double kAngle180 = M_PI;
    constexpr double kAngle270 = 1.5 * M_PI;
    constexpr double kAngle360 = 2.0 * M_PI;
  }

  VolumeBar::VolumeBar()
  {
    set_focusable(true);
    set_can_target(true);

    // Drag: Uses offsets relative to start
    auto const drag = Gtk::GestureDrag::create();
    drag->signal_drag_begin().connect([this](double, double) { _dragStartVolume = _volume; });
    drag->signal_drag_update().connect([this](double offsetX, double) { handleDragUpdate(offsetX); });
    add_controller(drag);

    // Click: Immediate jump to position
    auto const click = Gtk::GestureClick::create();
    click->signal_pressed().connect([this](int, double offsetX, double) { handleAbsoluteClick(offsetX); });
    add_controller(click);

    // Scroll
    auto const scroll = Gtk::EventControllerScroll::create();
    scroll->set_flags(Gtk::EventControllerScroll::Flags::VERTICAL);
    scroll->signal_scroll().connect(
      [this](double /*dx*/, double dy)
      {
        handleScroll(0.0, dy);
        return true;
      },
      false);
    add_controller(scroll);
  }

  VolumeBar::~VolumeBar() = default;

  void VolumeBar::setVolume(float volume)
  {
    auto const clamped = std::clamp(volume, 0.0F, 1.0F);
    if (std::abs(_volume - clamped) > kVolumeEpsilon)
    {
      _volume = clamped;
      queue_draw();
    }
  }

  float VolumeBar::getVolume() const
  {
    return _volume;
  }

  VolumeBar::VolumeChangedSignal& VolumeBar::signalVolumeChanged()
  {
    return _volumeChanged;
  }

  Gtk::SizeRequestMode VolumeBar::get_request_mode_vfunc() const
  {
    return Gtk::SizeRequestMode::CONSTANT_SIZE;
  }

  void VolumeBar::measure_vfunc(Gtk::Orientation orientation,
                                int /*for_size*/,
                                int& minimum,
                                int& natural,
                                int& /*minimum_baseline*/,
                                int& /*natural_baseline*/) const
  {
    if (orientation == Gtk::Orientation::HORIZONTAL)
    {
      minimum = kMinHorizontalWidth;
      natural = kNaturalHorizontalWidth; // Golden ratio width
    }
    else
    {
      minimum = kMinVerticalHeight;
      natural = kNaturalVerticalHeight; // Golden ratio height
    }
  }

  void VolumeBar::snapshot_vfunc(Glib::RefPtr<Gtk::Snapshot> const& snapshot)
  {
    auto const width = get_width();
    auto const height = get_height();

    auto const cr =
      snapshot->append_cairo(Gdk::Graphene::Rect(0, 0, static_cast<float>(width), static_cast<float>(height)));

    float const segmentWidth =
      (static_cast<float>(width) - (static_cast<float>(kNumSegments) - 1.0F) * static_cast<float>(kSegmentGap)) /
      static_cast<float>(kNumSegments);
    auto const context = get_style_context();
    auto const color = context->get_color();

    // Internal Padding for "Breathing Room"
    float const vPadding = kVerticalPadding;
    float const drawHeight = std::max(kMinDrawHeight, static_cast<float>(height) - (2.0F * vPadding));
    float const yOffset = vPadding;

    // Dynamically lookup the theme's accent/selection color
    auto activeColor = Gdk::RGBA{};
    if (!context->lookup_color("accent_color", activeColor))
    {
      if (!context->lookup_color("theme_selected_bg_color", activeColor))
      {
        // Fallback to a nice blue if theme doesn't provide named colors
        activeColor.set_rgba(kFallbackRed, kFallbackGreen, kFallbackBlue, kFallbackAlpha);
      }
    }

    // 1. Create the clipping path (10 rounded segments)
    // This defines the "containers" that will slice our triangle
    cr->save();
    cr->begin_new_path();
    for (int i = 0; i < kNumSegments; ++i)
    {
      float const segmentX = static_cast<float>(i) * (segmentWidth + static_cast<float>(kSegmentGap));
      // We add independent sub-paths for each rounded rect segment
      cr->begin_new_sub_path();
      cr->arc(segmentX + kSegmentRadius, yOffset + kSegmentRadius, kSegmentRadius, kAngle180, kAngle270);
      cr->arc(segmentX + segmentWidth - kSegmentRadius, yOffset + kSegmentRadius, kSegmentRadius, kAngle270, kAngle360);
      cr->arc(
        segmentX + segmentWidth - kSegmentRadius, yOffset + drawHeight - kSegmentRadius, kSegmentRadius, 0, kAngle90);
      cr->arc(segmentX + kSegmentRadius, yOffset + drawHeight - kSegmentRadius, kSegmentRadius, kAngle90, kAngle180);
      cr->close_path();
    }
    cr->clip();

    // 2. Define the "Perfect Triangle" path (a trapezoid from 10% to 100% height)
    auto const drawTrapezoid = [&](float currentWidth)
    {
      cr->begin_new_path();
      cr->move_to(0, yOffset + drawHeight);            // Bottom Left
      cr->line_to(currentWidth, yOffset + drawHeight); // Bottom Right
      float const hAtW =
        drawHeight * (kMinHeightFactor + kMaxHeightFactor * (currentWidth / static_cast<float>(width)));
      cr->line_to(currentWidth, yOffset + drawHeight - hAtW);
      cr->line_to(0, yOffset + drawHeight - (drawHeight * kMinHeightFactor));
      cr->close_path();
    };

    // 3. Draw Background (Inactive)
    drawTrapezoid(static_cast<float>(width));
    cr->set_source_rgba(color.get_red(), color.get_green(), color.get_blue(), kBackgroundOpacity);
    cr->fill();

    // 4. Draw Foreground (Active) - Clipped horizontally by volume
    if (_volume > 0.0F)
    {
      drawTrapezoid(static_cast<float>(width) * _volume);
      // Use the dynamically discovered theme color
      cr->set_source_rgba(activeColor.get_red(), activeColor.get_green(), activeColor.get_blue(), 1.0);
      cr->fill();
    }

    cr->restore();
  }

  void VolumeBar::handleAbsoluteClick(double offsetX)
  {
    auto const width = get_width();
    if (width <= 0)
    {
      return;
    }

    float const vol = std::clamp(static_cast<float>(offsetX / static_cast<double>(width)), 0.0F, 1.0F);
    setVolume(vol);
    _volumeChanged.emit(_volume);
  }

  void VolumeBar::handleDragUpdate(double offsetX)
  {
    auto const width = get_width();
    if (width <= 0)
    {
      return;
    }

    float const delta = static_cast<float>(offsetX / static_cast<double>(width));
    float const vol = std::clamp(_dragStartVolume + delta, 0.0F, 1.0F);
    setVolume(vol);
    _volumeChanged.emit(_volume);
  }

  void VolumeBar::handleScroll(double /*dx*/, double dy)
  {
    float const delta = (dy > 0) ? -0.05F : 0.05F;
    float const newVol = std::clamp(_volume + delta, 0.0F, 1.0F);
    if (std::abs(_volume - newVol) > kVolumeEpsilon)
    {
      _volume = newVol;
      _volumeChanged.emit(_volume);
      queue_draw();
    }
  }
} // namespace ao::gtk
