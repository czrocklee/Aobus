// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <gdkmm/rgba.h>
#include <gtkmm/widget.h>

#include <cstdint>
#include <memory>
#include <numbers>

namespace ao::gtk
{
  class AobusSoul final : public Gtk::Widget
  {
  public:
    AobusSoul();
    ~AobusSoul() override;

    AobusSoul(AobusSoul const&) = delete;
    AobusSoul& operator=(AobusSoul const&) = delete;
    AobusSoul(AobusSoul&&) = delete;
    AobusSoul& operator=(AobusSoul&&) = delete;

    void breathe(bool breathing);
    void setAura(Gdk::RGBA const& aura);
    void setShowFullLogo(bool show);

    static constexpr double kGoldenRatio = std::numbers::phi;

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
    struct PathDeleter final
    {
      void operator()(::GskPath* path) const noexcept;
    };

    struct StrokeDeleter final
    {
      void operator()(::GskStroke* stroke) const noexcept;
    };

    static Gdk::RGBA shiftColor(Gdk::RGBA const& color, float shift) noexcept;

    static constexpr double kFullCircleDegrees = 360.0;
    static constexpr double kBreathingPeriodSec = 5.119;
    static constexpr double kRotationPeriodSec = kBreathingPeriodSec * kGoldenRatio;
    static constexpr double kOpacityPeriodSec = kRotationPeriodSec * kGoldenRatio;
    static constexpr double kHuePeriodSec = kOpacityPeriodSec * kGoldenRatio;
    static constexpr double kStrokeWidthBase = 9.0;
    static constexpr double kStrokeWidthVariance = kStrokeWidthBase * (kGoldenRatio - 1.0);
    static constexpr float kRefHeight = 65.0F;
    static constexpr float kStrokeWidthA = 10.0F;
    static constexpr float kPhaseShift = 0.5F;
    static constexpr int kFullLogoMinSize = 54;
    static constexpr int kSoulMinSize = 24;

    Gdk::RGBA _cyan{"#00E5FF"};
    Gdk::RGBA _gray{"#6B7280"};
    Gdk::RGBA _amber{"#F97316"};
    Gdk::RGBA _aura{_cyan};

    double _timeSec = 0.0;
    bool _isStopped = true;
    bool _showFullLogo = false;

    std::int64_t _firstFrameTime = 0;
    std::uint32_t _tickId = 0;
    bool _isBreathing = false;

    std::unique_ptr<::GskPath, PathDeleter> _unitPathO{};
    std::unique_ptr<::GskPath, PathDeleter> _unitPathA{};
    std::unique_ptr<::GskStroke, StrokeDeleter> _cachedStroke{};
    std::unique_ptr<::GskStroke, StrokeDeleter> _cachedStrokeA{};
  };
} // namespace ao::gtk
