// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SemanticComponentRegistrations.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"

#include <gtkmm/button.h>
#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  namespace
  {
    /**
     * @brief library.openLibraryButton
     */
    class OpenLibraryButton final : public ILayoutComponent
    {
    public:
      OpenLibraryButton(LayoutContext& /*ctx*/, LayoutNode const& /*node*/)
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

    std::unique_ptr<ILayoutComponent> createOpenLibraryButton(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<OpenLibraryButton>(ctx, node);
    }
  } // namespace

  void registerOpenLibraryButtonComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "library.openLibraryButton",
                                .displayName = "Open Library Button",
                                .category = "Library",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createOpenLibraryButton);
  }
} // namespace ao::gtk::layout
