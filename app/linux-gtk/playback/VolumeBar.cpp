// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/VolumeBar.h"

#include <ao/uimodel/playback/output/VolumeViewModel.h>

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
    constexpr float kMinDrawThickness = 2.0F;
    constexpr float kBackgroundOpacity = 0.15F;
    constexpr float kMinThicknessFactor = 0.1F;
    constexpr float kMaxThicknessFactor = 0.9F;

    constexpr double kAngle90 = 0.5 * std::numbers::pi;
    constexpr double kAngle180 = std::numbers::pi;
    constexpr double kAngle270 = 1.5 * std::numbers::pi;
    constexpr double kAngle360 = 2.0 * std::numbers::pi;
    constexpr float kFullOpacity = 1.0F;
  } // namespace

  VolumeBar::VolumeBar()
  {
    add_css_class("ao-volume-bar");
    set_focusable(true);
    set_can_target(true);

    // Drag
    auto const dragPtr = Gtk::GestureDrag::create();
    dragPtr->signal_drag_begin().connect([this](double, double) { _dragStartVolume = _volume; });
    dragPtr->signal_drag_update().connect([this](double xPosition, double yPosition)
                                          { handleDragUpdate(xPosition, yPosition); });
    add_controller(dragPtr);

    // Click
    auto const clickPtr = Gtk::GestureClick::create();
    clickPtr->signal_pressed().connect([this](std::int32_t, double xPosition, double yPosition)
                                       { handleAbsoluteClick(xPosition, yPosition); });
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

  void VolumeBar::setOrientation(Gtk::Orientation orientation)
  {
    if (_orientation != orientation)
    {
      _orientation = orientation;
      queue_resize();
    }
  }

  Gtk::Orientation VolumeBar::orientation() const
  {
    return _orientation;
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
    return (_orientation == Gtk::Orientation::HORIZONTAL) ? Gtk::SizeRequestMode::WIDTH_FOR_HEIGHT
                                                          : Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH;
  }

  void VolumeBar::measure_vfunc(Gtk::Orientation orientation,
                                int forSize,
                                int& minimum,
                                int& natural,
                                int& /*minimumBaseline*/,
                                int& /*naturalBaseline*/) const
  {
    static constexpr double kAspectRatio = std::numbers::phi + 1; // ~2.618
    static constexpr int kMinThickness = 8;
    static constexpr int kMinLength = 20;

    static constexpr std::int32_t kDefaultThickness = 24;
    static constexpr std::int32_t kDefaultNaturalLength = 50;

    // RULE: Minimums must be small and constant to avoid GTK measurement contradictions
    // and prevent 'forcing' the container to grow.
    if (orientation == Gtk::Orientation::HORIZONTAL)
    {
      minimum = kMinLength;

      if (_orientation == Gtk::Orientation::HORIZONTAL)
      {
        // Horizontal bar asking for WIDTH. forSize is HEIGHT.
        natural = (forSize >= 0)
                    ? static_cast<std::int32_t>(std::round(static_cast<double>(forSize) * kAspectRatio))
                    : static_cast<std::int32_t>(std::round(static_cast<double>(kDefaultThickness) * kAspectRatio));
      }
      else
      {
        // Vertical bar asking for WIDTH. forSize is HEIGHT.
        natural = (forSize >= 0) ? static_cast<std::int32_t>(std::round(static_cast<double>(forSize) / kAspectRatio))
                                 : kMinThickness;
      }
    }
    else // VERTICAL
    {
      minimum = kMinThickness;

      if (_orientation == Gtk::Orientation::VERTICAL)
      {
        // Vertical bar asking for HEIGHT. forSize is WIDTH.
        natural = (forSize >= 0) ? static_cast<std::int32_t>(std::round(static_cast<double>(forSize) * kAspectRatio))
                                 : kDefaultNaturalLength;
      }
      else
      {
        // Horizontal bar asking for HEIGHT. forSize is WIDTH.
        natural = (forSize >= 0) ? static_cast<std::int32_t>(std::round(static_cast<double>(forSize) / kAspectRatio))
                                 : kDefaultThickness;
      }
    }

    natural = std::max(natural, minimum);
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
    float const drawWidth = std::max(0.0F, static_cast<float>(width) - (2.0F * hPadding));
    float const drawHeight = std::max(kMinDrawThickness, static_cast<float>(height) - (2.0F * vPadding));

    static constexpr float kSoulStrokeRatio = 9.0F / 89.124F;
    float const thickness = (_orientation == Gtk::Orientation::HORIZONTAL) ? drawHeight : drawWidth;
    float const length = (_orientation == Gtk::Orientation::HORIZONTAL) ? drawWidth : drawHeight;

    float const segmentThickness = thickness * kSoulStrokeRatio;
    float const minGap = std::max(1.5F, segmentThickness * 0.6F);

    std::int32_t segmentCount = 1;
    float segmentGap = 0.0F;

    if (length > segmentThickness)
    {
      float const rawSegments = (length + minGap) / (segmentThickness + minGap);
      segmentCount = std::max<std::int32_t>(1, static_cast<std::int32_t>(std::floor(rawSegments)));

      if (segmentCount > 1)
      {
        segmentGap =
          (length - (static_cast<float>(segmentCount) * segmentThickness)) / static_cast<float>(segmentCount - 1);
      }
    }

    float const segmentRadius = segmentThickness * 0.08F;

    auto activeColor = Gdk::RGBA{};

    if (!contextPtr->lookup_color("accent_color", activeColor))
    {
      contextPtr->lookup_color("theme_selected_bg_color", activeColor);
    }

    crPtr->save();
    crPtr->begin_new_path();

    for (std::int32_t idx = 0; idx < segmentCount; ++idx)
    {
      float const position = (static_cast<float>(idx) * (segmentThickness + segmentGap));
      crPtr->begin_new_sub_path();

      if (_orientation == Gtk::Orientation::HORIZONTAL)
      {
        float const segmentX = hPadding + position;
        crPtr->arc(segmentX + segmentRadius, vPadding + segmentRadius, segmentRadius, kAngle180, kAngle270);
        crPtr->arc(
          segmentX + segmentThickness - segmentRadius, vPadding + segmentRadius, segmentRadius, kAngle270, kAngle360);
        crPtr->arc(segmentX + segmentThickness - segmentRadius,
                   vPadding + drawHeight - segmentRadius,
                   segmentRadius,
                   0,
                   kAngle90);
        crPtr->arc(segmentX + segmentRadius, vPadding + drawHeight - segmentRadius, segmentRadius, kAngle90, kAngle180);
      }
      else
      {
        float const segmentY = vPadding + drawHeight - position - segmentThickness;
        crPtr->arc(hPadding + segmentRadius, segmentY + segmentRadius, segmentRadius, kAngle180, kAngle270);
        crPtr->arc(hPadding + drawWidth - segmentRadius, segmentY + segmentRadius, segmentRadius, kAngle270, kAngle360);
        crPtr->arc(hPadding + drawWidth - segmentRadius,
                   segmentY + segmentThickness - segmentRadius,
                   segmentRadius,
                   0,
                   kAngle90);
        crPtr->arc(
          hPadding + segmentRadius, segmentY + segmentThickness - segmentRadius, segmentRadius, kAngle90, kAngle180);
      }

      crPtr->close_path();
    }

    crPtr->clip();

    auto const drawRamp = [&](float progress)
    {
      crPtr->begin_new_path();

      if (_orientation == Gtk::Orientation::HORIZONTAL)
      {
        float const currentWidth = drawWidth * progress;
        crPtr->move_to(hPadding, vPadding + drawHeight);
        crPtr->line_to(hPadding + currentWidth, vPadding + drawHeight);
        float const hAtW = drawHeight * (kMinThicknessFactor + kMaxThicknessFactor * (currentWidth / drawWidth));
        crPtr->line_to(hPadding + currentWidth, vPadding + drawHeight - hAtW);
        crPtr->line_to(hPadding, vPadding + drawHeight - (drawHeight * kMinThicknessFactor));
      }
      else
      {
        float const currentHeight = drawHeight * progress;
        crPtr->rectangle(hPadding, vPadding + drawHeight - currentHeight, drawWidth, currentHeight);
      }

      crPtr->close_path();
    };

    crPtr->set_source_rgba(color.get_red(), color.get_green(), color.get_blue(), kBackgroundOpacity);
    drawRamp(1.0F);
    crPtr->fill();

    if (_volume > 0.0F)
    {
      crPtr->set_source_rgba(activeColor.get_red(), activeColor.get_green(), activeColor.get_blue(), kFullOpacity);
      drawRamp(_volume);
      crPtr->fill();
    }

    crPtr->restore();
  }

  void VolumeBar::handleAbsoluteClick(double xPosition, double yPosition)
  {
    float const hPadding = static_cast<float>(get_style_context()->get_padding().get_left());
    float const vPadding = static_cast<float>(get_style_context()->get_padding().get_top());

    if (_orientation == Gtk::Orientation::HORIZONTAL)
    {
      float const drawWidth = static_cast<float>(get_width()) - (2.0F * hPadding);

      if (drawWidth > 0)
      {
        _volume = std::clamp(static_cast<float>(xPosition - hPadding) / drawWidth, 0.0F, 1.0F);
      }
    }
    else
    {
      float const drawHeight = static_cast<float>(get_height()) - (2.0F * vPadding);

      if (drawHeight > 0)
      {
        _volume = std::clamp(1.0F - (static_cast<float>(yPosition - vPadding) / drawHeight), 0.0F, 1.0F);
      }
    }

    _volumeChanged.emit(_volume);
    updateTooltip();
    queue_draw();
  }

  void VolumeBar::handleDragUpdate(double xPosition, double yPosition)
  {
    float const hPadding = static_cast<float>(get_style_context()->get_padding().get_left());
    float const vPadding = static_cast<float>(get_style_context()->get_padding().get_top());

    if (_orientation == Gtk::Orientation::HORIZONTAL)
    {
      float const drawWidth = static_cast<float>(get_width()) - (2.0F * hPadding);

      if (drawWidth > 0)
      {
        _volume = std::clamp(_dragStartVolume + (static_cast<float>(xPosition) / drawWidth), 0.0F, 1.0F);
      }
    }
    else
    {
      float const drawHeight = static_cast<float>(get_height()) - (2.0F * vPadding);

      if (drawHeight > 0)
      {
        _volume = std::clamp(_dragStartVolume - (static_cast<float>(yPosition) / drawHeight), 0.0F, 1.0F);
      }
    }

    _volumeChanged.emit(_volume);
    updateTooltip();
    queue_draw();
  }

  void VolumeBar::handleScroll(double /*dx*/, double dy)
  {
    float const newVol = ao::uimodel::VolumeViewModel::resolveVolumeScroll(_volume, dy);

    if (std::abs(_volume - newVol) > kVolumeEpsilon)
    {
      _volume = newVol;
      _volumeChanged.emit(_volume);
      queue_draw();
    }
  }
} // namespace ao::gtk
