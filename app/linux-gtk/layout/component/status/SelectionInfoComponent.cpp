// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StatusComponentRegistrations.h"
#include "app/GtkUiDependencies.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "track/SelectionInfoLabel.h"
#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/layout/component/SharedLayoutComponentType.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    class SelectionInfoComponent final : public LayoutComponent
    {
    public:
      SelectionInfoComponent(LayoutBuildContext& ctx, LayoutNode const& /*node*/)
        : _widget{ctx.runtime.views(), ctx.dependencies.textCatalog}
      {
        _widget.widget().add_css_class("ao-selection-info-modern");
      }

      Gtk::Widget& widget() override { return _widget.widget(); }

    private:
      SelectionInfoLabel _widget;
    };

    std::unique_ptr<LayoutComponent> createSelectionInfo(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<SelectionInfoComponent>(ctx, node);
    }
  } // namespace

  void registerSelectionInfoComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      sharedComponentDescriptor(SharedLayoutComponentType::StatusSelectionInfo), createSelectionInfo);
  }
} // namespace ao::gtk::layout
