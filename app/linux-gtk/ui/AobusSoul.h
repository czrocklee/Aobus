// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Types.h>
#include <gdkmm/rgba.h>
#include <gtkmm/widget.h>
#include <memory>

namespace ao::gtk
{
  /**
   * @class AobusSoul
   * @brief The animated brand mark of Aobus.
   *
   * A high-performance, GPU-accelerated widget that renders the pulsing,
   * rotating Aobus logo. It reflects audio quality and transport state
   * through its color and animation.
   */
  class AobusSoul final : public Gtk::Widget
  {
  public:
    AobusSoul();
    ~AobusSoul() override;

    AobusSoul(AobusSoul const&) = delete;
    AobusSoul& operator=(AobusSoul const&) = delete;

    /**
     * @brief Update the visual state of the soul.
     * @param timeSec Current animation time in seconds.
     * @param quality Audio quality to determine the color.
     * @param isStopped Whether the engine is stopped (affects animation and color).
     * @param isReady Whether the engine is ready.
     */
    void update(double timeSec, ao::audio::Quality quality, bool isStopped, bool isReady);

    /**
     * @brief Set whether to show the full Aobus logo (a + o).
     * @param show True to show full logo, false for just the soul (o).
     */
    void setShowFullLogo(bool show);

    // Animation constants
    static constexpr double kGoldenRatio = 1.61803398875;
    static constexpr double kFullCircleDegrees = 360.0;
    static constexpr double kBreathingPeriodSec = 5.119;
    static constexpr double kRotationPeriodSec = kBreathingPeriodSec * kGoldenRatio;
    static constexpr double kOpacityPeriodSec = kRotationPeriodSec * kGoldenRatio;
    static constexpr double kHuePeriodSec = kOpacityPeriodSec * kGoldenRatio; // Aura Flow!
    static constexpr double kStrokeWidthBase = 9.0;
    static constexpr double kStrokeWidthVariance = kStrokeWidthBase * (kGoldenRatio - 1.0); // Golden Expansion!
    static constexpr double kPhaseShift = 0.5;

    static constexpr float kRefHeight = 65.0F;
    static constexpr float kStrokeWidthA = 10.0F;

    // Size constants
    static constexpr int kFullLogoMinSize = 54;
    static constexpr int kSoulMinSize = 24;

  protected:
    void snapshot_vfunc(Glib::RefPtr<Gtk::Snapshot> const& snapshot) override;
    Gtk::SizeRequestMode get_request_mode_vfunc() const override;
    void measure_vfunc(Gtk::Orientation orientation,
                       int for_size,
                       int& minimum,
                       int& natural,
                       int& minimum_baseline,
                       int& natural_baseline) const override;

  private:
    struct ColorCache
    {
      Gdk::RGBA cyan;
      Gdk::RGBA gray;
      Gdk::RGBA purple;
      Gdk::RGBA green;
      Gdk::RGBA orange;
      Gdk::RGBA red;
      Gdk::RGBA amber;
    };

    struct PathDeleter
    {
      void operator()(::GskPath* path) const noexcept;
    };

    struct StrokeDeleter
    {
      void operator()(::GskStroke* stroke) const noexcept;
    };

    static Gdk::RGBA shiftColor(Gdk::RGBA const& color, float shift) noexcept;

    ColorCache _colors;

    double _timeSec = 0.0;
    ao::audio::Quality _quality = ao::audio::Quality::Unknown;
    bool _isStopped = true;
    bool _isReady = false;
    bool _showFullLogo = false;

    // GSK Rendering Cache (Normalized unit paths)
    std::unique_ptr<::GskPath, PathDeleter> _unitPathO;
    std::unique_ptr<::GskPath, PathDeleter> _unitPathA;
    std::unique_ptr<::GskStroke, StrokeDeleter> _cachedStroke;
    std::unique_ptr<::GskStroke, StrokeDeleter> _cachedStrokeA;
  };
} // namespace ao::gtk
