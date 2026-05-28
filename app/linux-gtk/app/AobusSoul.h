// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/AobusSoulViewModel.h>

#include <gdkmm/rgba.h>
#include <glibmm/refptr.h>
#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

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

    static Gdk::RGBA mapAuraColor(uimodel::playback::AuraColor color);

    static constexpr double kGoldenRatio = std::numbers::phi;

  protected:
    void snapshot_vfunc(Glib::RefPtr<Gtk::Snapshot> const& snapshot) override;
    Gtk::SizeRequestMode get_request_mode_vfunc() const override;
    void measure_vfunc(Gtk::Orientation orientation,
                       int forSize,
                       int& minimum,
                       int& natural,
                       int& minimumBaseline,
                       int& naturalBaseline) const override;

  private:
    friend class AobusSoulTestPeer;

    struct Impl;

    static Gdk::RGBA shiftColor(Gdk::RGBA const& color, float shift) noexcept;

    std::unique_ptr<Impl> _impl;
  };
} // namespace ao::gtk
