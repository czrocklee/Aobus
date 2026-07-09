// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <glibmm/refptr.h>
#include <gtkmm/enums.h>
#include <gtkmm/snapshot.h>
#include <gtkmm/widget.h>
#include <sigc++/signal.h>

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

    VolumeBar(VolumeBar const&) = delete;
    VolumeBar& operator=(VolumeBar const&) = delete;
    VolumeBar(VolumeBar&&) = delete;
    VolumeBar& operator=(VolumeBar&&) = delete;

    void setVolume(float volume);
    float volume() const;

    void setIsHardwareAssisted(bool hw);

    void setOrientation(Gtk::Orientation orientation);
    Gtk::Orientation orientation() const;

    VolumeChangedSignal& signalVolumeChanged();

  protected:
    Gtk::SizeRequestMode get_request_mode_vfunc() const override;
    void measure_vfunc(Gtk::Orientation orientation,
                       int forSize,
                       int& minimum,
                       int& natural,
                       int& minimumBaseline,
                       int& naturalBaseline) const override;
    void snapshot_vfunc(Glib::RefPtr<Gtk::Snapshot> const& snapshotPtr) override;

  private:
    void updateTooltip();
    void handleAbsoluteClick(double xPosition, double yPosition);
    void handleDragUpdate(double xPosition, double yPosition);
    void handleScroll(double dx, double dy);

    float _volume = 1.0F;
    float _dragStartVolume = 0.0F;
    bool _isHardwareAssisted = false;
    Gtk::Orientation _orientation = Gtk::Orientation::HORIZONTAL;
    VolumeChangedSignal _volumeChanged;
  };
} // namespace ao::gtk
