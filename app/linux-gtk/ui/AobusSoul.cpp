// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AobusSoul.h"
#include <algorithm>
#include <cmath>
#include <format>
#include <gdkmm/graphene_point.h>
#include <gdkmm/graphene_rect.h>
#include <gtk/gtk.h>
#include <gtkmm/snapshot.h>
#include <numbers>
#include <string>

namespace ao::gtk
{
  AobusSoul::AobusSoul()
  {
    set_can_focus(false);
    set_focusable(false);

    _colors.cyan = Gdk::RGBA{"#00E5FF"};
    _colors.gray = Gdk::RGBA{"#6B7280"};
    _colors.purple = Gdk::RGBA{"#A855F7"};
    _colors.green = Gdk::RGBA{"#10B981"};
    _colors.orange = Gdk::RGBA{"#F59E0B"};
    _colors.red = Gdk::RGBA{"#EF4444"};
    _colors.amber = Gdk::RGBA{"#F97316"};

    // Initialize stroke once
    _cachedStroke = reinterpret_cast<_GskStroke*>(gsk_stroke_new(1.0f));
    gsk_stroke_set_line_cap(reinterpret_cast<GskStroke*>(_cachedStroke), GSK_LINE_CAP_ROUND);
  }

  AobusSoul::~AobusSoul()
  {
    if (_cachedPath)
    {
      gsk_path_unref(reinterpret_cast<GskPath*>(_cachedPath));
    }
    if (_cachedStroke)
    {
      gsk_stroke_free(reinterpret_cast<GskStroke*>(_cachedStroke));
    }
  }

  void AobusSoul::update(double timeSec, ao::audio::Quality quality, bool isStopped, bool isReady)
  {
    if (_timeSec == timeSec && _quality == quality && _isStopped == isStopped && _isReady == isReady)
    {
      return;
    }

    _timeSec = timeSec;
    _quality = quality;
    _isStopped = isStopped;
    _isReady = isReady;
    queue_draw();
  }

  void AobusSoul::set_show_full_logo(bool show)
  {
    if (_showFullLogo == show) return;
    _showFullLogo = show;
    queue_draw();
  }

  Gtk::SizeRequestMode AobusSoul::get_request_mode_vfunc() const
  {
    return Gtk::SizeRequestMode::CONSTANT_SIZE;
  }

  void AobusSoul::measure_vfunc(Gtk::Orientation orientation, int, int& minimum, int& natural, int&, int&) const
  {
    if (orientation == Gtk::Orientation::HORIZONTAL && _showFullLogo)
    {
      // Aspect ratio from SVG: 147 / 65 approx 2.26
      minimum = 54;
      natural = 54;
    }
    else
    {
      minimum = 24;
      natural = 24;
    }
  }

  void AobusSoul::snapshot_vfunc(Glib::RefPtr<Gtk::Snapshot> const& snapshot)
  {
    auto const width = get_width();
    auto const height = get_height();
    if (width <= 0 || height <= 0)
    {
      return;
    }

    // Color Logic
    auto indicatorColor = _isStopped ? _colors.cyan : _colors.gray;

    if (!_isReady)
    {
      indicatorColor = _colors.gray;
    }
    else if (!_isStopped)
    {
      switch (_quality)
      {
        case ao::audio::Quality::BitwisePerfect:
        case ao::audio::Quality::LosslessPadded: indicatorColor = _colors.purple; break;
        case ao::audio::Quality::LosslessFloat: indicatorColor = _colors.green; break;
        case ao::audio::Quality::LinearIntervention: indicatorColor = _colors.orange; break;
        case ao::audio::Quality::LossySource: indicatorColor = _colors.gray; break;
        case ao::audio::Quality::Clipped: indicatorColor = _colors.red; break;
        default: break;
      }
    }

    float const centerX = width / 2.0f;
    float const centerY = height / 2.0f;

    // SVG Proportions (Reference Height = 65, Radius = 30)
    float const H = static_cast<float>(height);
    float const radius = H * (30.0f / 65.0f);

    float const oCenterX = _showFullLogo ? centerX + (43.5f / 65.0f) * H : centerX;
    float const aCenterX = centerX - (43.5f / 65.0f) * H;

    // 1. Draw 'a' if requested
    if (_showFullLogo)
    {
      snapshot->save();

      auto* const builder = gsk_path_builder_new();
      graphene_point_t aCenter = {aCenterX, centerY};
      gsk_path_builder_add_circle(builder, &aCenter, radius);

      float const stemX = aCenterX + radius;
      float const stemY1 = centerY - (35.0f / 65.0f) * H;
      float const stemY2 = centerY + (30.0f / 65.0f) * H;

      gsk_path_builder_move_to(builder, stemX, stemY1);
      gsk_path_builder_line_to(builder, stemX, stemY2);

      auto* const aPath = gsk_path_builder_free_to_path(builder);
      float const aStrokeWidth = (10.0f / 65.0f) * H;
      auto* const aStroke = gsk_stroke_new(aStrokeWidth);
      gsk_stroke_set_line_cap(aStroke, GSK_LINE_CAP_ROUND);

      gtk_snapshot_push_stroke(snapshot->gobj(), aPath, aStroke);
      graphene_rect_t aBounds = {aCenterX - radius * 2.0f, centerY - radius * 2.0f, radius * 4.5f, radius * 4.0f};
      gtk_snapshot_append_color(snapshot->gobj(), _colors.amber.gobj(), &aBounds);
      gtk_snapshot_pop(snapshot->gobj());

      gsk_stroke_free(aStroke);
      gsk_path_unref(aPath);

      snapshot->restore();
    }

    // 2. Draw 'o' (Soul)
    float rotationAngle = 0.0f;
    float currentStrokeBase = 9.0f;
    float currentOpacity = 1.0f;

    if (!_isStopped)
    {
      rotationAngle =
        static_cast<float>(std::fmod(_timeSec * (kFullCircleDegrees / kRotationPeriodSec), kFullCircleDegrees));
      double const breathingPhase =
        std::fmod(_timeSec * (2.0 * std::numbers::pi / kBreathingPeriodSec), 2.0 * std::numbers::pi);
      constexpr double kPhaseShift = 0.5;
      currentStrokeBase = static_cast<float>(
        kStrokeWidthBase + (kStrokeWidthVariance * (std::sin(breathingPhase) * kPhaseShift + kPhaseShift)));

      double const opacityPhase =
        std::fmod(_timeSec * (2.0 * std::numbers::pi / kOpacityPeriodSec), 2.0 * std::numbers::pi);
      // Golden Ratio Opacity: range [0.618, 1.0]
      currentOpacity = static_cast<float>(0.809 + (0.191 * std::sin(opacityPhase)));
    }

    float const oStrokeWidth = (currentStrokeBase / 65.0f) * H;

    snapshot->save();
    snapshot->translate({oCenterX, centerY});
    snapshot->rotate(rotationAngle);

    if (currentOpacity < 0.999f)
    {
      gtk_snapshot_push_opacity(snapshot->gobj(), currentOpacity);
    }

    // Manage Cached Path (Full circle for 'o' as per SVG)
    if (!_cachedPath || std::abs(_cachedRadius - radius) > 0.001f)
    {
      if (_cachedPath) gsk_path_unref(reinterpret_cast<GskPath*>(_cachedPath));
      _cachedRadius = radius;

      auto* const oBuilder = gsk_path_builder_new();
      graphene_point_t oCenter = {0, 0};
      gsk_path_builder_add_circle(oBuilder, &oCenter, radius);
      _cachedPath = reinterpret_cast<_GskPath*>(gsk_path_builder_free_to_path(oBuilder));
    }

    gsk_stroke_set_line_width(reinterpret_cast<GskStroke*>(_cachedStroke), oStrokeWidth);

    gtk_snapshot_push_stroke(
      snapshot->gobj(), reinterpret_cast<GskPath*>(_cachedPath), reinterpret_cast<GskStroke*>(_cachedStroke));

    // Calculate Aura Flow (Hue Shift)
    float hueShift = 0.0f;
    if (!_isStopped)
    {
      float const huePhase =
        std::fmod(static_cast<float>(_timeSec) * (2.0f * std::numbers::pi_v<float> / static_cast<float>(kHuePeriodSec)),
                  2.0f * std::numbers::pi_v<float>);
      hueShift = 10.0f * std::sin(huePhase); // Subtle ±10 degree shift
    }

    auto const shiftColor = [](Gdk::RGBA const& color, float shift) -> Gdk::RGBA
    {
      if (std::abs(shift) < 0.01f) return color;

      float r = color.get_red();
      float g = color.get_green();
      float b = color.get_blue();
      float max = std::max({r, g, b}), min = std::min({r, g, b});
      float h, s, v = max, d = max - min;
      s = max == 0 ? 0 : d / max;
      if (max == min)
        h = 0;
      else
      {
        if (max == r)
          h = (g - b) / d + (g < b ? 6 : 0);
        else if (max == g)
          h = (b - r) / d + 2;
        else
          h = (r - g) / d + 4;
        h /= 6;
      }
      h = std::fmod(h + shift / 360.0f, 1.0f);
      if (h < 0) h += 1.0f;
      int i = static_cast<int>(h * 6);
      float f = h * 6 - i, p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
      switch (i % 6)
      {
        case 0: r = v, g = t, b = p; break;
        case 1: r = q, g = v, b = p; break;
        case 2: r = p, g = v, b = t; break;
        case 3: r = p, g = q, b = v; break;
        case 4: r = t, g = p, b = v; break;
        case 5: r = v, g = p, b = q; break;
      }
      return Gdk::RGBA(r, g, b, color.get_alpha());
    };

    GskColorStop stops[3];
    // Color Antagonism: Cyan shifts forward, Indicator shifts backward
    auto const shiftedCyan = shiftColor(_colors.cyan, hueShift);
    auto const shiftedIndicator = shiftColor(indicatorColor, -hueShift);

    // Reversed Gradient: Indicator (Quality) as the core (38.2%), Cyan as the dominant body (61.8%)
    stops[0].offset = 0.0f;
    stops[0].color = *(shiftedIndicator.gobj());
    stops[1].offset = 0.382f;
    stops[1].color = *(shiftedCyan.gobj());
    stops[2].offset = 1.0f;
    stops[2].color = *(shiftedCyan.gobj());

    float const outerRadius = radius + oStrokeWidth;
    graphene_rect_t gradientBounds = {{-outerRadius, -outerRadius}, {outerRadius * 2.0f, outerRadius * 2.0f}};
    graphene_point_t startPoint = {-radius, -radius};
    graphene_point_t endPoint = {radius, radius};

    gtk_snapshot_append_linear_gradient(snapshot->gobj(), &gradientBounds, &startPoint, &endPoint, stops, 3);
    gtk_snapshot_pop(snapshot->gobj());

    if (currentOpacity < 0.999f)
    {
      gtk_snapshot_pop(snapshot->gobj());
    }

    snapshot->restore();
  }
} // namespace ao::gtk
