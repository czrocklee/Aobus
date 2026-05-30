// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/ComponentTooltipController.h"
#include "layout/runtime/ILayoutComponent.h"

#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  class DecoratedLayoutComponent final : public ILayoutComponent
  {
  public:
    DecoratedLayoutComponent(std::unique_ptr<ILayoutComponent> contentPtr,
                             std::unique_ptr<ILayoutComponent> tooltipPtr);
    ~DecoratedLayoutComponent() override = default;

    DecoratedLayoutComponent(DecoratedLayoutComponent const&) = delete;
    DecoratedLayoutComponent& operator=(DecoratedLayoutComponent const&) = delete;
    DecoratedLayoutComponent(DecoratedLayoutComponent&&) = delete;
    DecoratedLayoutComponent& operator=(DecoratedLayoutComponent&&) = delete;

    Gtk::Widget& widget() override;

  private:
    std::unique_ptr<ILayoutComponent> _contentPtr;
    std::unique_ptr<ILayoutComponent> _tooltipPtr;
    ComponentTooltipController _tooltipController;
  };
} // namespace ao::gtk::layout
