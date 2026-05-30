// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/ILayoutComponent.h"

#include <gtkmm/tooltip.h>
#include <gtkmm/widget.h>

#include <cstdint>

namespace ao::gtk::layout
{
  class ComponentTooltipController final
  {
  public:
    ComponentTooltipController();
    ~ComponentTooltipController() = default;

    ComponentTooltipController(ComponentTooltipController const&) = delete;
    ComponentTooltipController& operator=(ComponentTooltipController const&) = delete;
    ComponentTooltipController(ComponentTooltipController&&) = delete;
    ComponentTooltipController& operator=(ComponentTooltipController&&) = delete;

    void attach(Gtk::Widget& target, ILayoutComponent& tooltipComponent);

  private:
    bool onQueryTooltip(std::int32_t xCoord,
                        std::int32_t yCoord,
                        bool keyboardTooltip,
                        Glib::RefPtr<Gtk::Tooltip> const& tooltip);

    Gtk::Widget* _target = nullptr;
    ILayoutComponent* _tooltipComponent = nullptr;
  };
} // namespace ao::gtk::layout
