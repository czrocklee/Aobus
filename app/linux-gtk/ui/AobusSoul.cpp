// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AobusSoul.h"
#include <algorithm>
#include <cmath>
#include <gdkmm/graphene_point.h>
#include <gdkmm/graphene_rect.h>
#include <gtk/gtk.h>
#include <gtkmm/snapshot.h>
#include <numbers>

namespace ao::gtk
{
  void AobusSoul::PathDeleter::operator()(::GskPath* path) const noexcept
  {
    ::gsk_path_unref(path);
  }

  void AobusSoul::StrokeDeleter::operator()(::GskStroke* stroke) const noexcept
  {
    ::gsk_stroke_free(stroke);
  }

  AobusSoul::AobusSoul()
    : _cachedStroke{::gsk_stroke_new(1.0F)}, _cachedStrokeA{::gsk_stroke_new(kStrokeWidthA / kRefHeight)}
  {
    set_can_focus(false);
    set_focusable(false);

    _colors = {.cyan = Gdk::RGBA{"#00E5FF"},
               .gray = Gdk::RGBA{"#6B7280"},
               .purple = Gdk::RGBA{"#A855F7"},
               .green = Gdk::RGBA{"#10B981"},
               .orange = Gdk::RGBA{"#F59E0B"},
               .red = Gdk::RGBA{"#EF4444"},
               .amber = Gdk::RGBA{"#F97316"}};

    ::gsk_stroke_set_line_cap(_cachedStroke.get(), GSK_LINE_CAP_ROUND);
    ::gsk_stroke_set_line_cap(_cachedStrokeA.get(), GSK_LINE_CAP_ROUND);

    static constexpr float kRefRadius = 30.0F;
    static constexpr float kNormalizedRadius = kRefRadius / kRefHeight;

    // 1. Pre-bake unit path for 'o' (Soul)
    auto* const oBuilder = ::gsk_path_builder_new();
    ::graphene_point_t const origin = {.x = 0.0F, .y = 0.0F};

    ::gsk_path_builder_add_circle(oBuilder, &origin, kNormalizedRadius);
    _unitPathO.reset(::gsk_path_builder_free_to_path(oBuilder));

    // 2. Pre-bake unit path for 'a' (Circle + Stem)
    auto* const aBuilder = ::gsk_path_builder_new();

    ::gsk_path_builder_add_circle(aBuilder, &origin, kNormalizedRadius);

    static constexpr float kRefStemY1 = -35.0F / kRefHeight;
    static constexpr float kRefStemY2 = 30.0F / kRefHeight;
    ::gsk_path_builder_move_to(aBuilder, kNormalizedRadius, kRefStemY1);
    ::gsk_path_builder_line_to(aBuilder, kNormalizedRadius, kRefStemY2);
    _unitPathA.reset(::gsk_path_builder_free_to_path(aBuilder));
  }

  AobusSoul::~AobusSoul() = default;

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

  void AobusSoul::setShowFullLogo(bool show)
  {
    if (_showFullLogo == show)
    {
      return;
    }

    _showFullLogo = show;
    queue_draw();
  }

  Gtk::SizeRequestMode AobusSoul::get_request_mode_vfunc() const
  {
    return Gtk::SizeRequestMode::CONSTANT_SIZE;
  }

  void AobusSoul::measure_vfunc(Gtk::Orientation orientation,
                                int /*for_size*/,
                                int& minimum,
                                int& natural,
                                int& /*minimum_baseline*/,
                                int& /*natural_baseline*/) const
  {
    if (orientation == Gtk::Orientation::HORIZONTAL && _showFullLogo)
    {
      // Aspect ratio from SVG: 147 / 65 approx 2.26
      minimum = kFullLogoMinSize;
      natural = kFullLogoMinSize;
    }
    else
    {
      minimum = kSoulMinSize;
      natural = kSoulMinSize;
    }
  }

  Gdk::RGBA AobusSoul::shiftColor(Gdk::RGBA const& color, float shift) noexcept
  {
    static constexpr float kMinShift = 0.01F;
    if (std::abs(shift) < kMinShift)
    {
      return color;
    }

    float const red = color.get_red();
    float const green = color.get_green();
    float const blue = color.get_blue();
    float const maxValue = std::max({red, green, blue});
    float const minValue = std::min({red, green, blue});
    float const delta = maxValue - minValue;
    float const saturation = (maxValue == 0.0F) ? 0.0F : (delta / maxValue);
    float hue = 0.0F;

    if (maxValue == minValue)
    {
      hue = 0.0F;
    }
    else
    {
      static constexpr float kSectorOffset6 = 6.0F;
      static constexpr float kSectorOffset2 = 2.0F;
      static constexpr float kSectorOffset4 = 4.0F;

      if (maxValue == red)
      {
        hue = ((green - blue) / delta) + (green < blue ? kSectorOffset6 : 0.0F);
      }
      else if (maxValue == green)
      {
        hue = ((blue - red) / delta) + kSectorOffset2;
      }
      else
      {
        hue = ((red - green) / delta) + kSectorOffset4;
      }

      hue /= kSectorOffset6;
    }

    static constexpr float kDegreesFullCircle = 360.0F;
    hue = std::fmod(hue + (shift / kDegreesFullCircle), 1.0F);
    if (hue < 0.0F)
    {
      hue += 1.0F;
    }

    static constexpr float kSectorCountF = 6.0F;
    static constexpr int kSectorCount = 6;
    int const sector = static_cast<int>(hue * kSectorCountF);
    float const fraction = (hue * kSectorCountF) - static_cast<float>(sector);
    float const pValue = maxValue * (1.0F - saturation);
    float const qValue = maxValue * (1.0F - (fraction * saturation));
    float const tValue = maxValue * (1.0F - ((1.0F - fraction) * saturation));

    float resultRed = 0.0F;
    float resultGreen = 0.0F;
    float resultBlue = 0.0F;

    switch (sector % kSectorCount)
    {
      case 0:
        resultRed = maxValue;
        resultGreen = tValue;
        resultBlue = pValue;
        break;
      case 1:
        resultRed = qValue;
        resultGreen = maxValue;
        resultBlue = pValue;
        break;
      case 2:
        resultRed = pValue;
        resultGreen = maxValue;
        resultBlue = tValue;
        break;
      case 3: // NOLINT(readability-magic-numbers)
        resultRed = pValue;
        resultGreen = qValue;
        resultBlue = maxValue;
        break;
      case 4: // NOLINT(readability-magic-numbers)
        resultRed = tValue;
        resultGreen = pValue;
        resultBlue = maxValue;
        break;
      case 5: // NOLINT(readability-magic-numbers)
      default:
        resultRed = maxValue;
        resultGreen = pValue;
        resultBlue = qValue;
        break;
    }

    return Gdk::RGBA(resultRed, resultGreen, resultBlue, color.get_alpha());
  }

  void AobusSoul::snapshot_vfunc(Glib::RefPtr<Gtk::Snapshot> const& snapshot)
  {
    auto const width = static_cast<float>(get_width());
    auto const height = static_cast<float>(get_height());
    if (width <= 0.0F || height <= 0.0F)
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

    float const centerX = width / 2.0F;
    float const centerY = height / 2.0F;

    // SVG Proportions (Reference Height = 65)
    static constexpr float kRefHeight = 65.0F;
    static constexpr float kRefXOffset = 43.5F;
    static constexpr float kNormalizedXOffset = kRefXOffset / kRefHeight;

    float const oCenterX = _showFullLogo ? (centerX + (kNormalizedXOffset * height)) : centerX;
    float const aCenterX = centerX - (kNormalizedXOffset * height);

    // 1. Draw 'a' if requested
    if (_showFullLogo)
    {
      snapshot->save();
      snapshot->translate({aCenterX, centerY});
      snapshot->scale(height, height);

      ::gtk_snapshot_push_stroke(snapshot->gobj(), _unitPathA.get(), _cachedStrokeA.get());

      static constexpr float kRefR = 30.0F / 65.0F;
      static constexpr float kRefBoundsX = -kRefR * 2.0F;
      static constexpr float kRefBoundsY = -kRefR * 2.0F;
      static constexpr float kRefBoundsW = kRefR * 4.5F;
      static constexpr float kRefBoundsH = kRefR * 4.0F;
      ::graphene_rect_t const aBounds = {
        .origin = {.x = kRefBoundsX, .y = kRefBoundsY}, .size = {.width = kRefBoundsW, .height = kRefBoundsH}};

      ::gtk_snapshot_append_color(snapshot->gobj(), _colors.amber.gobj(), &aBounds);
      ::gtk_snapshot_pop(snapshot->gobj());

      snapshot->restore();
    }

    // 2. Draw 'o' (Soul)
    float rotationAngle = 0.0F;
    float currentStrokeBase = static_cast<float>(kStrokeWidthBase);
    float currentOpacity = 1.0F;

    if (!_isStopped)
    {
      rotationAngle =
        static_cast<float>(std::fmod(_timeSec * (kFullCircleDegrees / kRotationPeriodSec), kFullCircleDegrees));
      double const breathingPhase =
        std::fmod(_timeSec * (2.0 * std::numbers::pi / kBreathingPeriodSec), 2.0 * std::numbers::pi);
      currentStrokeBase = static_cast<float>(
        kStrokeWidthBase + (kStrokeWidthVariance * ((std::sin(breathingPhase) * kPhaseShift) + kPhaseShift)));

      double const opacityPhase =
        std::fmod(_timeSec * (2.0 * std::numbers::pi / kOpacityPeriodSec), 2.0 * std::numbers::pi);
      // Golden Ratio Opacity: range [0.618, 1.0]
      static constexpr double kOpacityBase = 0.809;
      static constexpr double kOpacityVariance = 0.191;
      currentOpacity = static_cast<float>(kOpacityBase + (kOpacityVariance * std::sin(opacityPhase)));
    }

    ::gsk_stroke_set_line_width(_cachedStroke.get(), currentStrokeBase / kRefHeight);

    snapshot->save();
    snapshot->translate({oCenterX, centerY});
    snapshot->rotate(rotationAngle);
    snapshot->scale(height, height);

    static constexpr float kOpacityThreshold = 0.999F;
    if (currentOpacity < kOpacityThreshold)
    {
      ::gtk_snapshot_push_opacity(snapshot->gobj(), currentOpacity);
    }

    ::gtk_snapshot_push_stroke(snapshot->gobj(), _unitPathO.get(), _cachedStroke.get());

    // Calculate Aura Flow (Hue Shift)
    float hueShift = 0.0F;
    if (!_isStopped)
    {
      float const huePhase =
        std::fmod(static_cast<float>(_timeSec) * (2.0F * std::numbers::pi_v<float> / static_cast<float>(kHuePeriodSec)),
                  2.0F * std::numbers::pi_v<float>);
      static constexpr float kMaxHueShift = 10.0F;
      hueShift = kMaxHueShift * std::sin(huePhase); // Subtle ±10 degree shift
    }

    static constexpr std::size_t kStopCount = 3;
    auto stops = std::array<::GskColorStop, kStopCount>{};
    // Color Antagonism: Cyan shifts forward, Indicator shifts backward
    auto const shiftedCyan = shiftColor(_colors.cyan, hueShift);
    auto const shiftedIndicator = shiftColor(indicatorColor, -hueShift);

    // Player UI: Cyan as the core (38.2%), Indicator (Quality) as the dominant body (61.8%)
    static constexpr float kCoreOffset = 0.382F;
    stops[0].offset = 0.0F;
    stops[0].color = *(shiftedCyan.gobj());
    stops[1].offset = kCoreOffset;
    stops[1].color = *(shiftedIndicator.gobj());
    stops[2].offset = 1.0F;
    stops[2].color = *(shiftedIndicator.gobj());

    static constexpr float kUnitR = 30.0F / 65.0F;
    float const normalizedStrokeWidth = currentStrokeBase / kRefHeight;
    float const outerRadius = kUnitR + normalizedStrokeWidth;
    ::graphene_rect_t const gradientBounds = {.origin = {.x = -outerRadius, .y = -outerRadius},
                                              .size = {.width = outerRadius * 2.0F, .height = outerRadius * 2.0F}};
    ::graphene_point_t const startPoint = {.x = kUnitR, .y = kUnitR};
    ::graphene_point_t const endPoint = {.x = -kUnitR, .y = -kUnitR};

    ::gtk_snapshot_append_linear_gradient(
      snapshot->gobj(), &gradientBounds, &startPoint, &endPoint, stops.data(), stops.size());
    ::gtk_snapshot_pop(snapshot->gobj());

    if (currentOpacity < kOpacityThreshold)
    {
      ::gtk_snapshot_pop(snapshot->gobj());
    }

    snapshot->restore();
  }
} // namespace ao::gtk
