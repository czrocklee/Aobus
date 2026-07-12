// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SemanticComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
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
      OpenLibraryButton(LayoutBuildContext& /*ctx*/, LayoutNode const& /*node*/)
      {
        _button.set_label("Open Library...");
        _button.set_icon_name("folder-open-symbolic");
        _button.signal_clicked().connect(
          []
          {
            // This usually triggers a dialog in MainWindow
          });
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
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createOpenLibraryButton);
  }
} // namespace ao::gtk::layout
