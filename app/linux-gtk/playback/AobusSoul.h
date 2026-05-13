// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Types.h>
#include <gdkmm/rgba.h>
#include <gtkmm/widget.h>
#include <memory>

#include "runtime/CorePrimitives.h"

namespace ao::rt
{
  class AppSession;
}

namespace ao::gtk
{
  /**
   * @class AobusSoul
   * @brief The animated brand mark of Aobus.
   */
  class AobusSoul final : public Gtk::Widget
  {
  public:
    AobusSoul();
    ~AobusSoul() override;

    AobusSoul(AobusSoul const&) = delete;
    AobusSoul& operator=(AobusSoul const&) = delete;

    void bind(ao::rt::AppSession& session);
    void unbind();
    void update(double timeSec, ao::audio::Quality quality, bool isStopped, bool isReady);
    void setShowFullLogo(bool show);

    static constexpr double kGoldenRatio = 1.61803398875;

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
    struct ColorCache final
    {
      Gdk::RGBA cyan{};
      Gdk::RGBA gray{};
      Gdk::RGBA purple{};
      Gdk::RGBA green{};
      Gdk::RGBA orange{};
      Gdk::RGBA red{};
      Gdk::RGBA amber{};
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

    ColorCache _colors{};
    double _timeSec = 0.0;
    ao::audio::Quality _quality = ao::audio::Quality::Unknown;
    bool _isStopped = true;
    bool _isReady = false;
    bool _showFullLogo = false;

    std::int64_t _firstFrameTime = 0;
    std::uint32_t _tickId = 0;
    bool _isPlaying = false;
    ao::rt::Subscription _qualitySub{};
    ao::rt::Subscription _idleSub{};
    ao::rt::Subscription _startedSub{};
    ao::rt::Subscription _stoppedSub{};

    std::unique_ptr<::GskPath, PathDeleter> _unitPathO{};
    std::unique_ptr<::GskPath, PathDeleter> _unitPathA{};
    std::unique_ptr<::GskStroke, StrokeDeleter> _cachedStroke{};
    std::unique_ptr<::GskStroke, StrokeDeleter> _cachedStrokeA{};
  };
} // namespace ao::gtk
