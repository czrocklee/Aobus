// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/AobusSoul.h"

#include <ao/uimodel/playback/AobusSoulViewModel.h>

#include <gdkmm/frameclock.h>
#include <gdkmm/graphene_point.h>
#include <gdkmm/graphene_rect.h>
#include <glibmm/refptr.h>
#include <gsk/gsk.h>
#include <gtk/gtk.h>
#include <gtkmm/enums.h>
#include <gtkmm/snapshot.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>

namespace ao::gtk
{
  namespace
  {
    struct PathDeleter final
    {
      void operator()(::GskPath* path) const noexcept { ::gsk_path_unref(path); }
    };

    struct StrokeDeleter final
    {
      void operator()(::GskStroke* stroke) const noexcept { ::gsk_stroke_free(stroke); }
    };

    constexpr double kFullCircleDegrees = 360.0;
    constexpr double kBreathingPeriodSec = 5.119;
    constexpr double kRotationPeriodSec = kBreathingPeriodSec * AobusSoul::kGoldenRatio;
    constexpr double kOpacityPeriodSec = kRotationPeriodSec * AobusSoul::kGoldenRatio;
    constexpr double kHuePeriodSec = kOpacityPeriodSec * AobusSoul::kGoldenRatio;
    constexpr float kRefHeight = 65.0F;
    constexpr float kUnitRadius = 30.0F / kRefHeight;
    constexpr float kLogoXOffset = 43.5F / kRefHeight;
    constexpr float kStrokeWidthA = 10.0F;
    constexpr float kPhaseShift = 0.5F;
    constexpr float kCos60 = 0.5F;
    constexpr float kSin60 = 0.866F;
    constexpr float kEpsilon = 0.001F;
  } // namespace

  struct AobusSoul::Impl final
  {
    Gdk::RGBA cyan{"#00E5FF"};
    Gdk::RGBA gray{"#6B7280"};
    Gdk::RGBA amber{"#F97316"};
    Gdk::RGBA aura{cyan};

    double timeSec = 0.0;
    bool isStopped = true;
    bool showFullLogo = false;

    float baseStrokeWidth = 9.0F;
    float innerGlyphScale = 1.0F;

    std::int64_t firstFrameTime = 0;
    std::uint32_t tickId = 0;
    bool isBreathing = false;

    std::unique_ptr<::GskPath, PathDeleter> pathSigilPtr{};
    std::unique_ptr<::GskPath, PathDeleter> pathSealPtr{};
    InnerGlyph currentGlyph = InnerGlyph::None;

    std::unique_ptr<::GskPath, PathDeleter> unitPathOPtr{};
    std::unique_ptr<::GskPath, PathDeleter> unitPathAPtr{};
    std::unique_ptr<::GskStroke, StrokeDeleter> cachedStrokePtr{};
    std::unique_ptr<::GskStroke, StrokeDeleter> cachedStrokeAPtr{};
    std::unique_ptr<::GskStroke, StrokeDeleter> cachedStrokeGlyphPtr{};
  };

  AobusSoul::AobusSoul()
    : _implPtr{std::make_unique<Impl>()}
  {
    add_css_class("ao-soul");
    set_can_focus(false);
    set_focusable(false);

    _implPtr->cachedStrokePtr.reset(::gsk_stroke_new(1.0F));
    _implPtr->cachedStrokeAPtr.reset(::gsk_stroke_new(kStrokeWidthA / kRefHeight));
    _implPtr->cachedStrokeGlyphPtr.reset(::gsk_stroke_new(_implPtr->baseStrokeWidth / kRefHeight));

    ::gsk_stroke_set_line_cap(_implPtr->cachedStrokePtr.get(), GSK_LINE_CAP_ROUND);
    ::gsk_stroke_set_line_cap(_implPtr->cachedStrokeAPtr.get(), GSK_LINE_CAP_ROUND);
    ::gsk_stroke_set_line_cap(_implPtr->cachedStrokeGlyphPtr.get(), GSK_LINE_CAP_ROUND);
    ::gsk_stroke_set_line_join(_implPtr->cachedStrokeGlyphPtr.get(), GSK_LINE_JOIN_ROUND);

    static constexpr float kRefRadius = 30.0F;
    static constexpr float kNormalizedRadius = kRefRadius / kRefHeight;

    auto* const oBuilder = ::gsk_path_builder_new();
    auto const origin = ::graphene_point_t{.x = 0.0F, .y = 0.0F};

    ::gsk_path_builder_add_circle(oBuilder, &origin, kNormalizedRadius);
    _implPtr->unitPathOPtr.reset(::gsk_path_builder_free_to_path(oBuilder));

    auto* const aBuilder = ::gsk_path_builder_new();

    ::gsk_path_builder_add_circle(aBuilder, &origin, kNormalizedRadius);

    static constexpr float kRefStemY1 = -35.0F / kRefHeight;
    static constexpr float kRefStemY2 = 30.0F / kRefHeight;

    ::gsk_path_builder_move_to(aBuilder, kNormalizedRadius, kRefStemY1);
    ::gsk_path_builder_line_to(aBuilder, kNormalizedRadius, kRefStemY2);
    _implPtr->unitPathAPtr.reset(::gsk_path_builder_free_to_path(aBuilder));

    static constexpr float kInnerGlyphRadius = 14.0F / kRefHeight;

    // Sigil (Kinetic Triangle)
    auto* const sigilBuilder = ::gsk_path_builder_new();
    ::gsk_path_builder_move_to(sigilBuilder, kInnerGlyphRadius, 0.0F);
    ::gsk_path_builder_line_to(sigilBuilder, -kInnerGlyphRadius * kCos60, -kInnerGlyphRadius * kSin60);
    ::gsk_path_builder_line_to(sigilBuilder, -kInnerGlyphRadius * kCos60, kInnerGlyphRadius * kSin60);
    ::gsk_path_builder_close(sigilBuilder);
    _implPtr->pathSigilPtr.reset(::gsk_path_builder_free_to_path(sigilBuilder));

    // Seal (Balanced Bars)
    auto* const sealBuilder = ::gsk_path_builder_new();
    float const barX = kInnerGlyphRadius * 0.4F;
    float const barY = kInnerGlyphRadius * 0.7F;
    ::gsk_path_builder_move_to(sealBuilder, -barX, -barY);
    ::gsk_path_builder_line_to(sealBuilder, -barX, barY);
    ::gsk_path_builder_move_to(sealBuilder, barX, -barY);
    ::gsk_path_builder_line_to(sealBuilder, barX, barY);
    _implPtr->pathSealPtr.reset(::gsk_path_builder_free_to_path(sealBuilder));

    _implPtr->tickId = add_tick_callback(
      [this](Glib::RefPtr<Gdk::FrameClock> const& clock) -> bool
      {
        if (_implPtr->isBreathing)
        {
          auto const frameTime = clock->get_frame_time();

          if (_implPtr->firstFrameTime == 0)
          {
            _implPtr->firstFrameTime = frameTime;
          }

          static constexpr double kMicrosecondsPerSecond = 1'000'000.0;
          _implPtr->timeSec = static_cast<double>(frameTime - _implPtr->firstFrameTime) / kMicrosecondsPerSecond;
          _implPtr->isStopped = false;
        }
        else
        {
          _implPtr->timeSec = 0.0;
          _implPtr->isStopped = true;
        }

        queue_draw();
        return true;
      });
  }

  AobusSoul::~AobusSoul()
  {
    if (_implPtr && _implPtr->tickId != 0)
    {
      remove_tick_callback(_implPtr->tickId);
    }
  }

  void AobusSoul::breathe(bool const breathing)
  {
    if (_implPtr->isBreathing == breathing)
    {
      return;
    }

    _implPtr->isBreathing = breathing;

    if (!breathing)
    {
      _implPtr->firstFrameTime = 0;
    }

    queue_draw();
  }

  void AobusSoul::setInnerGlyph(InnerGlyph const glyph)
  {
    if (_implPtr->currentGlyph == glyph)
    {
      return;
    }

    _implPtr->currentGlyph = glyph;
    queue_draw();
  }

  void AobusSoul::setBaseStrokeWidth(float const width)
  {
    if (std::abs(_implPtr->baseStrokeWidth - width) < kEpsilon)
    {
      return;
    }

    _implPtr->baseStrokeWidth = width;

    _implPtr->cachedStrokeGlyphPtr.reset(::gsk_stroke_new(_implPtr->baseStrokeWidth / kRefHeight));
    ::gsk_stroke_set_line_cap(_implPtr->cachedStrokeGlyphPtr.get(), GSK_LINE_CAP_ROUND);
    ::gsk_stroke_set_line_join(_implPtr->cachedStrokeGlyphPtr.get(), GSK_LINE_JOIN_ROUND);

    queue_draw();
  }

  void AobusSoul::setInnerGlyphScale(float const scale)
  {
    if (std::abs(_implPtr->innerGlyphScale - scale) < kEpsilon)
    {
      return;
    }

    _implPtr->innerGlyphScale = scale;
    queue_draw();
  }

  void AobusSoul::setAura(Gdk::RGBA const& aura)
  {
    if (_implPtr->aura == aura)
    {
      return;
    }

    _implPtr->aura = aura;
    queue_draw();
  }

  void AobusSoul::setShowFullLogo(bool const show)
  {
    if (_implPtr->showFullLogo == show)
    {
      return;
    }

    _implPtr->showFullLogo = show;
    queue_draw();
  }

  Gdk::RGBA AobusSoul::mapAuraColor(ao::uimodel::playback::AuraColor color)
  {
    using Color = ao::uimodel::playback::AuraColor;

    switch (color)
    {
      case Color::Idle: return Gdk::RGBA{"#00E5FF"};
      case Color::Unknown: return Gdk::RGBA{"#6B7280"};
      case Color::Perfect: return Gdk::RGBA{"#A855F7"};
      case Color::Lossless: return Gdk::RGBA{"#10B981"};
      case Color::Intervention: return Gdk::RGBA{"#F59E0B"};
      case Color::Clipped: return Gdk::RGBA{"#EF4444"};
    }

    return Gdk::RGBA{"#6B7280"};
  }

  Gtk::SizeRequestMode AobusSoul::get_request_mode_vfunc() const
  {
    return Gtk::SizeRequestMode::CONSTANT_SIZE;
  }

  void AobusSoul::measure_vfunc(Gtk::Orientation /*orientation*/,
                                int /*for_size*/,
                                int& minimum,
                                int& natural,
                                int& /*minimum_baseline*/,
                                int& /*natural_baseline*/) const
  {
    // CSS controls the real glyph size via parent padding or min-width/min-height.
    minimum = 0;
    natural = 0;
  }

  Gdk::RGBA AobusSoul::shiftColor(Gdk::RGBA const& color, float const shift) noexcept
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
    static constexpr int kRedSector = 0;
    static constexpr int kYellowSector = 1;
    static constexpr int kGreenSector = 2;
    static constexpr int kCyanSector = 3;
    static constexpr int kBlueSector = 4;
    static constexpr int kMagentaSector = 5;
    int const sector = static_cast<std::int32_t>(hue * kSectorCountF);
    float const fraction = (hue * kSectorCountF) - static_cast<float>(sector);
    float const pValue = maxValue * (1.0F - saturation);
    float const qValue = maxValue * (1.0F - (fraction * saturation));
    float const tValue = maxValue * (1.0F - ((1.0F - fraction) * saturation));

    float resultRed = 0.0F;
    float resultGreen = 0.0F;
    float resultBlue = 0.0F;

    switch (sector % kSectorCount)
    {
      case kRedSector:
        resultRed = maxValue;
        resultGreen = tValue;
        resultBlue = pValue;
        break;
      case kYellowSector:
        resultRed = qValue;
        resultGreen = maxValue;
        resultBlue = pValue;
        break;
      case kGreenSector:
        resultRed = pValue;
        resultGreen = maxValue;
        resultBlue = tValue;
        break;
      case kCyanSector:
        resultRed = pValue;
        resultGreen = qValue;
        resultBlue = maxValue;
        break;
      case kBlueSector:
        resultRed = tValue;
        resultGreen = pValue;
        resultBlue = maxValue;
        break;
      case kMagentaSector:
      default:
        resultRed = maxValue;
        resultGreen = pValue;
        resultBlue = qValue;
        break;
    }

    return {resultRed, resultGreen, resultBlue, color.get_alpha()};
  }

  void AobusSoul::snapshot_vfunc(Glib::RefPtr<Gtk::Snapshot> const& snapshot)
  {
    auto const width = static_cast<float>(get_width());
    auto const height = static_cast<float>(get_height());

    if (width <= 0.0F || height <= 0.0F)
    {
      return;
    }

    auto const aura = Gdk::RGBA{_implPtr->isStopped ? _implPtr->cyan : _implPtr->aura};

    float const strokeWidthVariance = _implPtr->baseStrokeWidth * (static_cast<float>(AobusSoul::kGoldenRatio) - 1.0F);
    float const maxStrokeWidth = _implPtr->baseStrokeWidth + strokeWidthVariance;
    float const maxNormalizedStrokeWidth = maxStrokeWidth / kRefHeight;
    float const soulOuterRadius = kUnitRadius + (maxNormalizedStrokeWidth / 2.0F);

    float const horizontalRadius = _implPtr->showFullLogo ? (kLogoXOffset + soulOuterRadius) : soulOuterRadius;
    float const drawingScale = std::min(width / (horizontalRadius * 2.0F), height / (soulOuterRadius * 2.0F));

    float const centerX = width / 2.0F;
    float const centerY = height / 2.0F;

    float const oCenterX = _implPtr->showFullLogo ? (centerX + (kLogoXOffset * drawingScale)) : centerX;
    float const aCenterX = centerX - (kLogoXOffset * drawingScale);

    // 1. Draw 'a' if requested
    if (_implPtr->showFullLogo)
    {
      snapshot->save();
      snapshot->translate({aCenterX, centerY});
      snapshot->scale(drawingScale, drawingScale);

      ::gtk_snapshot_push_stroke(snapshot->gobj(), _implPtr->unitPathAPtr.get(), _implPtr->cachedStrokeAPtr.get());

      static constexpr float kRefBoundsX = -kUnitRadius * 2.0F;
      static constexpr float kRefBoundsY = -kUnitRadius * 2.0F;
      static constexpr float kRefBoundsW = kUnitRadius * 4.5F;
      static constexpr float kRefBoundsH = kUnitRadius * 4.0F;
      auto const aBounds = ::graphene_rect_t{
        .origin = {.x = kRefBoundsX, .y = kRefBoundsY}, .size = {.width = kRefBoundsW, .height = kRefBoundsH}};

      ::gtk_snapshot_append_color(snapshot->gobj(), _implPtr->amber.gobj(), &aBounds);
      ::gtk_snapshot_pop(snapshot->gobj());

      snapshot->restore();
    }

    // 2. Draw 'o' (Soul)
    float rotationAngle = 0.0F;
    float currentStrokeBase = _implPtr->baseStrokeWidth;
    float currentOpacity = 1.0F;

    if (!_implPtr->isStopped)
    {
      rotationAngle = static_cast<float>(
        std::fmod(_implPtr->timeSec * (kFullCircleDegrees / kRotationPeriodSec), kFullCircleDegrees));
      double const breathingPhase =
        std::fmod(_implPtr->timeSec * (2.0 * std::numbers::pi / kBreathingPeriodSec), 2.0 * std::numbers::pi);
      currentStrokeBase = static_cast<float>(
        _implPtr->baseStrokeWidth + (strokeWidthVariance * ((std::sin(breathingPhase) * kPhaseShift) + kPhaseShift)));

      double const opacityPhase =
        std::fmod(_implPtr->timeSec * (2.0 * std::numbers::pi / kOpacityPeriodSec), 2.0 * std::numbers::pi);
      // Golden Ratio Opacity: range [0.618, 1.0]
      static constexpr double kOpacityBase = 0.809;
      static constexpr double kOpacityVariance = 0.191;
      currentOpacity = static_cast<float>(kOpacityBase + (kOpacityVariance * std::sin(opacityPhase)));
    }

    ::gsk_stroke_set_line_width(_implPtr->cachedStrokePtr.get(), currentStrokeBase / kRefHeight);

    snapshot->save();
    snapshot->translate({oCenterX, centerY});
    snapshot->rotate(rotationAngle);
    snapshot->scale(drawingScale, drawingScale);

    static constexpr float kOpacityThreshold = 0.999F;

    if (currentOpacity < kOpacityThreshold)
    {
      ::gtk_snapshot_push_opacity(snapshot->gobj(), currentOpacity);
    }

    ::gtk_snapshot_push_stroke(snapshot->gobj(), _implPtr->unitPathOPtr.get(), _implPtr->cachedStrokePtr.get());

    // Calculate Aura Flow (Hue Shift)
    float hueShift = 0.0F;

    if (!_implPtr->isStopped)
    {
      float const huePhase = std::fmod(
        static_cast<float>(_implPtr->timeSec) * (2.0F * std::numbers::pi_v<float> / static_cast<float>(kHuePeriodSec)),
        2.0F * std::numbers::pi_v<float>);
      static constexpr float kMaxHueShift = 10.0F;
      hueShift = kMaxHueShift * std::sin(huePhase);
    }

    static constexpr std::size_t kStopCount = 3;
    auto stops = std::array<::GskColorStop, kStopCount>{};
    auto const shiftedCyan = shiftColor(_implPtr->cyan, hueShift);
    auto const shiftedAura = shiftColor(aura, -hueShift);

    // Player UI: Cyan as the core (38.2%), Indicator (Quality) as the dominant body (61.8%)
    static constexpr float kCoreOffset = 0.382F;
    stops[0].offset = 0.0F;
    stops[0].color = *(shiftedCyan.gobj());
    stops[1].offset = kCoreOffset;
    stops[1].color = *(shiftedAura.gobj());
    stops[2].offset = 1.0F;
    stops[2].color = *(shiftedAura.gobj());

    float const normalizedStrokeWidth = currentStrokeBase / kRefHeight;
    float const outerRadius = kUnitRadius + normalizedStrokeWidth;
    auto const gradientBounds = ::graphene_rect_t{.origin = {.x = -outerRadius, .y = -outerRadius},
                                                  .size = {.width = outerRadius * 2.0F, .height = outerRadius * 2.0F}};
    auto const startPoint = ::graphene_point_t{.x = kUnitRadius, .y = kUnitRadius};
    auto const endPoint = ::graphene_point_t{.x = -kUnitRadius, .y = -kUnitRadius};

    ::gtk_snapshot_append_linear_gradient(
      snapshot->gobj(), &gradientBounds, &startPoint, &endPoint, stops.data(), stops.size());
    ::gtk_snapshot_pop(snapshot->gobj());

    // 3. Draw Inner Glyph if present
    if (_implPtr->currentGlyph != InnerGlyph::None)
    {
      auto* const glyphPath =
        (_implPtr->currentGlyph == InnerGlyph::Sigil) ? _implPtr->pathSigilPtr.get() : _implPtr->pathSealPtr.get();

      // Glyphs are drawn without the Soul's rotation to remain readable, but they follow the breathing scale/opacity
      snapshot->save();
      snapshot->rotate(-rotationAngle); // Counter-rotate to stay upright
      snapshot->scale(_implPtr->innerGlyphScale, _implPtr->innerGlyphScale);

      if (_implPtr->currentGlyph == InnerGlyph::Sigil)
      {
        ::gtk_snapshot_push_stroke(snapshot->gobj(), glyphPath, _implPtr->cachedStrokeGlyphPtr.get());
        ::gtk_snapshot_append_linear_gradient(
          snapshot->gobj(), &gradientBounds, &startPoint, &endPoint, stops.data(), stops.size());
        ::gtk_snapshot_pop(snapshot->gobj());
      }
      else
      {
        // Seal (Bars) looks better with a solid indicator color or slightly different gradient
        ::gtk_snapshot_push_stroke(snapshot->gobj(), glyphPath, _implPtr->cachedStrokeGlyphPtr.get());
        ::gtk_snapshot_append_color(snapshot->gobj(), shiftedAura.gobj(), &gradientBounds);
        ::gtk_snapshot_pop(snapshot->gobj());
      }

      snapshot->restore();
    }

    if (currentOpacity < kOpacityThreshold)
    {
      ::gtk_snapshot_pop(snapshot->gobj());
    }

    snapshot->restore();
  }
} // namespace ao::gtk
