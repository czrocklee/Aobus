// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/VolumeBar.h"

#include <ao/uimodel/playback/VolumeViewModel.h>

#include <gdkmm/graphene_rect.h>
#include <gdkmm/rgba.h>
#include <glibmm/refptr.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/snapshot.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
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
    constexpr float kScrollStep = 0.05F;
    constexpr float kFullOpacity = 1.0F;
  }

  VolumeBar::VolumeBar()
  {
    add_css_class("ao-volume-bar");
    set_focusable(true);
    set_can_target(true);

    // Drag: Uses offsets relative to start
    auto const drag = Gtk::GestureDrag::create();
    drag->signal_drag_begin().connect([this](double, double) { _dragStartVolume = _volume; });
    drag->signal_drag_update().connect([this](double offsetX, double) { handleDragUpdate(offsetX); });
    add_controller(drag);

    // Click: Immediate jump to position
    auto const click = Gtk::GestureClick::create();
    click->signal_pressed().connect([this](std::int32_t, double offsetX, double) { handleAbsoluteClick(offsetX); });
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
    if (auto const clamped = std::clamp(volume, 0.0F, 1.0F); std::abs(_volume - clamped) > kVolumeEpsilon)
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

    auto const cr =
      snapshot->append_cairo(Gdk::Graphene::Rect{0, 0, static_cast<float>(width), static_cast<float>(height)});

    auto const context = get_style_context();
    auto const color = context->get_color();
    auto const cssPadding = context->get_padding();

    float const vPadding = static_cast<float>(cssPadding.get_top());
    float const hPadding = static_cast<float>(cssPadding.get_left());
    float const drawHeight = std::max(kMinDrawHeight, static_cast<float>(height) - (2.0F * vPadding));
    float const yOffset = vPadding;

    // Segment gap and radius scale with the available width.
    float const segmentGap = std::max(1.0F, (static_cast<float>(width) - 2.0F * hPadding) * 0.025F);
    float const segmentWidth =
      ((static_cast<float>(width) - 2.0F * hPadding) - (static_cast<float>(kNumSegments) - 1.0F) * segmentGap) /
      static_cast<float>(kNumSegments);
    float const segmentRadius = segmentWidth * 0.08F;

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

    for (std::int32_t idx = 0; idx < kNumSegments; ++idx)
    {
      float const segmentX = hPadding + (static_cast<float>(idx) * (segmentWidth + segmentGap));
      cr->begin_new_sub_path();
      cr->arc(segmentX + segmentRadius, yOffset + segmentRadius, segmentRadius, kAngle180, kAngle270);
      cr->arc(segmentX + segmentWidth - segmentRadius, yOffset + segmentRadius, segmentRadius, kAngle270, kAngle360);
      cr->arc(
        segmentX + segmentWidth - segmentRadius, yOffset + drawHeight - segmentRadius, segmentRadius, 0, kAngle90);
      cr->arc(segmentX + segmentRadius, yOffset + drawHeight - segmentRadius, segmentRadius, kAngle90, kAngle180);
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
      cr->set_source_rgba(activeColor.get_red(), activeColor.get_green(), activeColor.get_blue(), kFullOpacity);
      cr->fill();
    }

    cr->restore();
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
