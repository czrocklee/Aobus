// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SemanticComponentRegistrations.h"
#include "app/GtkUiDependencies.h"
#include "common/AccessibleLabel.h"
#include "i18n/GtkTextCatalog.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "portal/ImportExportActions.h"
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/button.h>
#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief library.openLibraryButton
     */
    class OpenLibraryButton final : public LayoutComponent
    {
    public:
      OpenLibraryButton(LayoutBuildContext& ctx, LayoutNode const& /*node*/)
      {
        auto const label = std::string{ctx.dependencies.gtkTextCatalog.text(GtkTextId::OpenLibrary)};
        _button.set_icon_name("folder-open-symbolic");
        setTooltipAndAccessibleLabel(_button, label);

        if (auto* const actions = ctx.dependencies.importExportActions; actions != nullptr)
        {
          _button.signal_clicked().connect([actions] { actions->openLibrary(); });
        }
        else
        {
          _button.set_sensitive(false);
        }
      }

      Gtk::Widget& widget() override { return _button; }

    private:
      Gtk::Button _button;
    };

    std::unique_ptr<LayoutComponent> createOpenLibraryButton(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<OpenLibraryButton>(ctx, node);
    }
  } // namespace

  void registerOpenLibraryButtonComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "library.openLibraryButton",
                                .displayName = "Open Library Button",
                                .category = LayoutComponentCategory::Library,
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createOpenLibraryButton);
  }
} // namespace ao::gtk::layout
