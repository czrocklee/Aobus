// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/AobusSoul.h"

#include <ao/uimodel/FrameClock.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <gdkmm/frameclock.h>
#include <gdkmm/graphene_point.h>
#include <gdkmm/graphene_rect.h>
#include <glibmm/refptr.h>
#include <gsk/gsk.h>
#include <gtk/gtk.h>
#include <gtkmm/enums.h>
#include <gtkmm/snapshot.h>
#include <gtkmm/widget.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

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

    constexpr float kRefHeight = 65.0F;
    constexpr float kUnitRadius = 30.0F / kRefHeight;
    constexpr float kLogoXOffset = 43.5F / kRefHeight;
    constexpr float kStrokeWidthA = 10.0F;
    constexpr float kCos60 = 0.5F;
    constexpr float kSin60 = 0.866F;
    constexpr float kEpsilon = 0.001F;

    Gdk::RGBA rgbaFromSoulRgb(uimodel::AobusSoulRgb const color, float const alpha = 1.0F) noexcept
    {
      constexpr float kMaxChannel = 255.0F;
      return {static_cast<float>(color.red) / kMaxChannel,
              static_cast<float>(color.green) / kMaxChannel,
              static_cast<float>(color.blue) / kMaxChannel,
              alpha};
    }
  } // namespace

  struct AobusSoul::Impl final
  {
    Gdk::RGBA cyan{rgbaFromSoulRgb(uimodel::kAobusSoulUiCyan)};
    Gdk::RGBA amber{rgbaFromSoulRgb(uimodel::kAobusSoulAnchorAmber)};
    Gdk::RGBA aura{cyan};

    std::chrono::duration<double> time{0.0};
    bool isStopped = true;
    bool shouldShowFullLogo = false;

    float baseStrokeWidth = 9.0F;
    float innerGlyphScale = 1.0F;

    std::optional<uimodel::FrameClock::TimePoint> optFirstFrameTime;
    std::uint32_t tickId = 0;
    bool isMapped = false;
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
  }

  AobusSoul::~AobusSoul()
  {
    stopTick();
  }

  void AobusSoul::startTickIfNeeded()
  {
    if (!_implPtr->isMapped || !_implPtr->isBreathing || _implPtr->tickId != 0)
    {
      return;
    }

    _implPtr->tickId = add_tick_callback(
      [this](Glib::RefPtr<Gdk::FrameClock> const& clockPtr) -> bool
      {
        auto const frameTime = uimodel::FrameClock::fromMicros(clockPtr->get_frame_time());

        if (!_implPtr->optFirstFrameTime)
        {
          _implPtr->optFirstFrameTime = frameTime;
        }

        _implPtr->time = frameTime - *_implPtr->optFirstFrameTime;
        _implPtr->isStopped = false;

        queue_draw();
        return true;
      });
  }

  void AobusSoul::stopTick()
  {
    if (_implPtr->tickId != 0)
    {
      remove_tick_callback(_implPtr->tickId);
      _implPtr->tickId = 0;
    }
  }

  bool AobusSoul::isBreathing() const
  {
    return _implPtr->isBreathing;
  }

  bool AobusSoul::isTickActive() const
  {
    return _implPtr->tickId != 0;
  }

  bool AobusSoul::shouldShowFullLogo() const
  {
    return _implPtr->shouldShowFullLogo;
  }

  Gdk::RGBA AobusSoul::aura() const
  {
    return _implPtr->aura;
  }

  void AobusSoul::breathe(bool const breathing)
  {
    if (_implPtr->isBreathing == breathing)
    {
      return;
    }

    _implPtr->isBreathing = breathing;

    if (breathing)
    {
      startTickIfNeeded();
    }
    else
    {
      stopTick();
      _implPtr->optFirstFrameTime.reset();
      _implPtr->time = std::chrono::duration<double>::zero();
      _implPtr->isStopped = true;
    }

    queue_draw();
  }

  void AobusSoul::on_map()
  {
    Gtk::Widget::on_map();
    _implPtr->isMapped = true;
    startTickIfNeeded();
  }

  void AobusSoul::on_unmap()
  {
    stopTick();
    _implPtr->isMapped = false;
    Gtk::Widget::on_unmap();
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
    if (_implPtr->shouldShowFullLogo == show)
    {
      return;
    }

    _implPtr->shouldShowFullLogo = show;
    queue_draw();
  }

  Gdk::RGBA AobusSoul::mapSoulAura(ao::uimodel::SoulAura aura)
  {
    return rgbaFromSoulRgb(uimodel::aobusSoulAuraRgb(aura));
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
    // The parent allocation controls the rendered glyph size.
    minimum = 0;
    natural = 0;
  }

  Gdk::RGBA AobusSoul::shiftColor(Gdk::RGBA const& color, float const shift) noexcept
  {
    constexpr float kMaxChannel = 255.0F;
    auto const rgb =
      uimodel::AobusSoulRgb{.red = static_cast<std::uint8_t>(std::lround(color.get_red() * kMaxChannel)),
                            .green = static_cast<std::uint8_t>(std::lround(color.get_green() * kMaxChannel)),
                            .blue = static_cast<std::uint8_t>(std::lround(color.get_blue() * kMaxChannel))};

    return rgbaFromSoulRgb(uimodel::aobusSoulShiftRgb(rgb, shift), color.get_alpha());
  }

  void AobusSoul::snapshot_vfunc(Glib::RefPtr<Gtk::Snapshot> const& snapshotPtr)
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

    float const horizontalRadius = _implPtr->shouldShowFullLogo ? (kLogoXOffset + soulOuterRadius) : soulOuterRadius;
    float const drawingScale = std::min(width / (horizontalRadius * 2.0F), height / (soulOuterRadius * 2.0F));

    float const centerX = width / 2.0F;
    float const centerY = height / 2.0F;

    float const oCenterX = _implPtr->shouldShowFullLogo ? (centerX + (kLogoXOffset * drawingScale)) : centerX;
    float const aCenterX = centerX - (kLogoXOffset * drawingScale);

    // 1. Draw 'a' if requested
    if (_implPtr->shouldShowFullLogo)
    {
      snapshotPtr->save();
      snapshotPtr->translate({aCenterX, centerY});
      snapshotPtr->scale(drawingScale, drawingScale);

      ::gtk_snapshot_push_stroke(snapshotPtr->gobj(), _implPtr->unitPathAPtr.get(), _implPtr->cachedStrokeAPtr.get());

      static constexpr float kRefBoundsX = -kUnitRadius * 2.0F;
      static constexpr float kRefBoundsY = -kUnitRadius * 2.0F;
      static constexpr float kRefBoundsW = kUnitRadius * 4.5F;
      static constexpr float kRefBoundsH = kUnitRadius * 4.0F;
      auto const aBounds = ::graphene_rect_t{
        .origin = {.x = kRefBoundsX, .y = kRefBoundsY}, .size = {.width = kRefBoundsW, .height = kRefBoundsH}};

      ::gtk_snapshot_append_color(snapshotPtr->gobj(), _implPtr->amber.gobj(), &aBounds);
      ::gtk_snapshot_pop(snapshotPtr->gobj());

      snapshotPtr->restore();
    }

    // 2. Draw 'o' (Soul)
    float rotationAngle = 0.0F;
    float currentStrokeBase = _implPtr->baseStrokeWidth;
    float currentOpacity = 1.0F;
    auto motion = uimodel::AobusSoulMotionFrame{};

    if (!_implPtr->isStopped)
    {
      motion = uimodel::aobusSoulMotionAt(_implPtr->time);
      rotationAngle = static_cast<float>(motion.rotationDegrees);
      currentStrokeBase = static_cast<float>(_implPtr->baseStrokeWidth + (strokeWidthVariance * motion.breath));
      currentOpacity = static_cast<float>(motion.luminance);
    }

    ::gsk_stroke_set_line_width(_implPtr->cachedStrokePtr.get(), currentStrokeBase / kRefHeight);

    snapshotPtr->save();
    snapshotPtr->translate({oCenterX, centerY});
    snapshotPtr->rotate(rotationAngle);
    snapshotPtr->scale(drawingScale, drawingScale);

    static constexpr float kOpacityThreshold = 0.999F;

    if (currentOpacity < kOpacityThreshold)
    {
      ::gtk_snapshot_push_opacity(snapshotPtr->gobj(), currentOpacity);
    }

    ::gtk_snapshot_push_stroke(snapshotPtr->gobj(), _implPtr->unitPathOPtr.get(), _implPtr->cachedStrokePtr.get());

    // Calculate Aura Flow (Hue Shift)
    float const hueShift = _implPtr->isStopped ? 0.0F : static_cast<float>(motion.hueShiftDegrees);

    static constexpr std::size_t kStopCount = 3;
    auto stops = std::array<::GskColorStop, kStopCount>{};
    auto const shiftedCyan = shiftColor(_implPtr->cyan, hueShift);
    auto const shiftedAura = shiftColor(aura, -hueShift);

    // Player UI: Cyan as the core (38.2%), Indicator (Quality) as the dominant body (61.8%)
    static constexpr float kCoreOffset = static_cast<float>(uimodel::kAobusSoulCoreGradientStop);
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
      snapshotPtr->gobj(), &gradientBounds, &startPoint, &endPoint, stops.data(), stops.size());
    ::gtk_snapshot_pop(snapshotPtr->gobj());

    // 3. Draw Inner Glyph if present
    if (_implPtr->currentGlyph != InnerGlyph::None)
    {
      auto* const glyphPath =
        (_implPtr->currentGlyph == InnerGlyph::Sigil) ? _implPtr->pathSigilPtr.get() : _implPtr->pathSealPtr.get();

      // Glyphs are drawn without the Soul's rotation to remain readable, but they follow the breathing scale/opacity
      snapshotPtr->save();
      snapshotPtr->rotate(-rotationAngle); // Counter-rotate to stay upright
      snapshotPtr->scale(_implPtr->innerGlyphScale, _implPtr->innerGlyphScale);

      if (_implPtr->currentGlyph == InnerGlyph::Sigil)
      {
        ::gtk_snapshot_push_stroke(snapshotPtr->gobj(), glyphPath, _implPtr->cachedStrokeGlyphPtr.get());
        ::gtk_snapshot_append_linear_gradient(
          snapshotPtr->gobj(), &gradientBounds, &startPoint, &endPoint, stops.data(), stops.size());
        ::gtk_snapshot_pop(snapshotPtr->gobj());
      }
      else
      {
        // Seal (Bars) looks better with a solid indicator color or slightly different gradient
        ::gtk_snapshot_push_stroke(snapshotPtr->gobj(), glyphPath, _implPtr->cachedStrokeGlyphPtr.get());
        ::gtk_snapshot_append_color(snapshotPtr->gobj(), shiftedAura.gobj(), &gradientBounds);
        ::gtk_snapshot_pop(snapshotPtr->gobj());
      }

      snapshotPtr->restore();
    }

    if (currentOpacity < kOpacityThreshold)
    {
      ::gtk_snapshot_pop(snapshotPtr->gobj());
    }

    snapshotPtr->restore();
  }
} // namespace ao::gtk
