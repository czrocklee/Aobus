// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <gtkmm.h>
#include <sigc++/sigc++.h>

namespace ao::gtk
{
  /**
   * @brief A custom volume bar widget with 12 segments and profile-based height ramping.
   */
  class VolumeBar final : public Gtk::Widget
  {
  public:
    using VolumeChangedSignal = sigc::signal<void(float)>;

    VolumeBar();
    ~VolumeBar() override;

    void setVolume(float volume);
    float getVolume() const;

    VolumeChangedSignal& signalVolumeChanged();

  protected:
    Gtk::SizeRequestMode get_request_mode_vfunc() const override;
    void measure_vfunc(Gtk::Orientation orientation,
                       int for_size,
                       int& minimum,
                       int& natural,
                       int& minimum_baseline,
                       int& natural_baseline) const override;
    void snapshot_vfunc(Glib::RefPtr<Gtk::Snapshot> const& snapshot) override;

  private:
    void handleAbsoluteClick(double x);
    void handleDragUpdate(double offsetX);
    void handleScroll(double dx, double dy);

    float _volume = 1.0F;
    float _dragStartVolume = 0.0F;
    VolumeChangedSignal _volumeChanged;

    static constexpr int kNumSegments = 10;
    static constexpr int kSegmentGap = 1;
    static constexpr float kSegmentRadius = 0.8F;
  };
} // namespace ao::gtk
