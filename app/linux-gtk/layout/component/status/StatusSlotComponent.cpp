// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StatusComponentRegistrations.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "track/StatusSlot.h"
#include <ao/rt/AppRuntime.h>

#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  namespace
  {
    class StatusSlotComponent final : public ILayoutComponent
    {
    public:
      StatusSlotComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _widget{ctx.runtime.mutation(), ctx.runtime.notifications(), ctx.runtime.views()}
      {
      }

      Gtk::Widget& widget() override { return _widget.widget(); }

    private:
      StatusSlot _widget;
    };

    std::unique_ptr<ILayoutComponent> createStatusSlot(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<StatusSlotComponent>(ctx, node);
    }
  } // namespace

  void registerStatusSlotComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "status.statusSlot", .displayName = "Status Slot", .category = "Status"}, createStatusSlot);
  }
} // namespace ao::gtk::layout
