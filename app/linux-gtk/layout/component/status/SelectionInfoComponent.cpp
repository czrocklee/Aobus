// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StatusComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "track/SelectionInfoLabel.h"
#include <ao/rt/ViewService.h>
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
      SelectionInfoComponent(rt::ViewService& views, i18n::MessageCatalog const& textCatalog)
        : _widget{views, textCatalog}
      {
        _widget.widget().add_css_class("ao-selection-info-modern");
      }

      Gtk::Widget& widget() override { return _widget.widget(); }

    private:
      SelectionInfoLabel _widget;
    };
  } // namespace

  void registerSelectionInfoComponent(ComponentRegistry& registry,
                                      rt::ViewService& views,
                                      i18n::MessageCatalog const& textCatalog)
  {
    registry.registerComponent(sharedComponentDescriptor(SharedLayoutComponentType::StatusSelectionInfo),
                               [&views, textCatalog](LayoutBuildContext const& /*ctx*/, LayoutNode const& /*node*/)
                               { return std::make_unique<SelectionInfoComponent>(views, textCatalog); });
  }
} // namespace ao::gtk::layout
