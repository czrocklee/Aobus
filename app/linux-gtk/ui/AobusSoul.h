// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Types.h>
#include <gdkmm/rgba.h>
#include <gtkmm/widget.h>

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
    void set_show_full_logo(bool show);

    // Animation constants
    static constexpr double kFullCircleDegrees = 360.0;
    static constexpr double kBreathingPeriodSec = 5.119;
    static constexpr double kRotationPeriodSec = kBreathingPeriodSec * 1.61803398875;
    static constexpr double kOpacityPeriodSec = kRotationPeriodSec * 1.61803398875;
    static constexpr double kHuePeriodSec = kOpacityPeriodSec * 1.61803398875; // Aura Flow!
    static constexpr double kStrokeWidthBase = 9.0;
    static constexpr double kStrokeWidthVariance = kStrokeWidthBase * (1.61803398875 - 1.0); // Golden Expansion!

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
    } _colors;

    double _timeSec = 0.0;
    ao::audio::Quality _quality = ao::audio::Quality::Unknown;
    bool _isStopped = true;
    bool _isReady = false;
    bool _showFullLogo = false;

    // GSK Rendering Cache
    struct _GskPath* _cachedPath = nullptr;
    struct _GskStroke* _cachedStroke = nullptr;
    float _cachedRadius = 0.0f;
  };
} // namespace ao::gtk
