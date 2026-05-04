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
  }

  VolumeBar::VolumeBar()
  {
    set_focusable(true);
    set_can_target(true);

    // Drag: Uses offsets relative to start
    auto const drag = Gtk::GestureDrag::create();
    drag->signal_drag_begin().connect([this](double, double) { _dragStartVolume = _volume; });
    drag->signal_drag_update().connect([this](double x, double) { handleDragUpdate(x); });
    add_controller(drag);

    // Click: Immediate jump to position
    auto const click = Gtk::GestureClick::create();
    click->signal_pressed().connect([this](int, double x, double) { handleAbsoluteClick(x); });
    add_controller(click);

    // Scroll
    auto const scroll = Gtk::EventControllerScroll::create();
    scroll->set_flags(Gtk::EventControllerScroll::Flags::VERTICAL);
    scroll->signal_scroll().connect(
      [this](double dx, double dy)
      {
        handleScroll(dx, dy);
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
      minimum = 32;
      natural = 42; // Golden ratio width
    }
    else
    {
      minimum = 20;
      natural = 26; // Golden ratio height
    }
  }

  void VolumeBar::snapshot_vfunc(Glib::RefPtr<Gtk::Snapshot> const& snapshot)
  {
    auto const width = get_width();
    auto const height = get_height();

    auto const cr =
      snapshot->append_cairo(Gdk::Graphene::Rect(0, 0, static_cast<float>(width), static_cast<float>(height)));

    float const segmentWidth = (static_cast<float>(width) - (kNumSegments - 1) * kSegmentGap) / kNumSegments;
    auto const context = get_style_context();
    auto const color = context->get_color();

    // Internal Padding for "Breathing Room"
    float const vPadding = 4.0f;
    float const drawHeight = std::max(2.0f, static_cast<float>(height) - 2.0f * vPadding);
    float const yOffset = vPadding;

    // Dynamically lookup the theme's accent/selection color
    Gdk::RGBA activeColor{};
    if (!context->lookup_color("accent_color", activeColor))
    {
      if (!context->lookup_color("theme_selected_bg_color", activeColor))
      {
        // Fallback to a nice blue if theme doesn't provide named colors
        activeColor.set_rgba(0.208, 0.518, 0.894, 1.0);
      }
    }

    // 1. Create the clipping path (10 rounded segments)
    // This defines the "containers" that will slice our triangle
    cr->save();
    cr->begin_new_path();
    for (int i = 0; i < kNumSegments; ++i)
    {
      float const x = i * (segmentWidth + kSegmentGap);
      // We add independent sub-paths for each rounded rect segment
      cr->begin_new_sub_path();
      cr->arc(x + kSegmentRadius, yOffset + kSegmentRadius, kSegmentRadius, M_PI, 1.5 * M_PI);
      cr->arc(x + segmentWidth - kSegmentRadius, yOffset + kSegmentRadius, kSegmentRadius, 1.5 * M_PI, 2.0 * M_PI);
      cr->arc(x + segmentWidth - kSegmentRadius, yOffset + drawHeight - kSegmentRadius, kSegmentRadius, 0, 0.5 * M_PI);
      cr->arc(x + kSegmentRadius, yOffset + drawHeight - kSegmentRadius, kSegmentRadius, 0.5 * M_PI, M_PI);
      cr->close_path();
    }
    cr->clip();

    // 2. Define the "Perfect Triangle" path (a trapezoid from 10% to 100% height)
    auto const drawTrapezoid = [&](float w)
    {
      cr->begin_new_path();
      cr->move_to(0, yOffset + drawHeight); // Bottom Left
      cr->line_to(w, yOffset + drawHeight); // Bottom Right
      float const hAtW = drawHeight * (0.1f + 0.9f * (w / width));
      cr->line_to(w, yOffset + drawHeight - hAtW);
      cr->line_to(0, yOffset + drawHeight - (drawHeight * 0.1f));
      cr->close_path();
    };

    // 3. Draw Background (Inactive)
    drawTrapezoid(static_cast<float>(width));
    cr->set_source_rgba(color.get_red(), color.get_green(), color.get_blue(), 0.15);
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

  void VolumeBar::handleAbsoluteClick(double x)
  {
    auto const width = get_width();
    if (width <= 0) return;

    float const vol = std::clamp(static_cast<float>(x / width), 0.0F, 1.0F);
    setVolume(vol);
    _volumeChanged.emit(_volume);
  }

  void VolumeBar::handleDragUpdate(double offsetX)
  {
    auto const width = get_width();
    if (width <= 0) return;

    float const delta = static_cast<float>(offsetX / width);
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
