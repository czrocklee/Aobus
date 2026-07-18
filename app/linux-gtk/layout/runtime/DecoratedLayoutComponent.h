// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/ComponentInteractionController.h"
#include "layout/runtime/ComponentTooltipController.h"
#include "layout/runtime/LayoutComponent.h"

#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  class DecoratedLayoutComponent final : public LayoutComponent
  {
  public:
    DecoratedLayoutComponent(std::unique_ptr<LayoutComponent> contentPtr,
                             std::unique_ptr<LayoutComponent> tooltipPtr,
                             std::unique_ptr<ComponentInteractionController> interactionPtr = nullptr);
    ~DecoratedLayoutComponent() override;

    DecoratedLayoutComponent(DecoratedLayoutComponent const&) = delete;
    DecoratedLayoutComponent& operator=(DecoratedLayoutComponent const&) = delete;
    DecoratedLayoutComponent(DecoratedLayoutComponent&&) = delete;
    DecoratedLayoutComponent& operator=(DecoratedLayoutComponent&&) = delete;

    Gtk::Widget& widget() override;

  private:
    std::unique_ptr<LayoutComponent> _contentPtr;
    std::unique_ptr<LayoutComponent> _tooltipPtr;
    ComponentTooltipController _tooltipController;
    std::unique_ptr<ComponentInteractionController> _interactionPtr;
  };
} // namespace ao::gtk::layout
