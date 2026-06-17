// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SemanticComponentRegistrations.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "list/ListNavigationController.h"
#include <ao/library/MusicLibrary.h>
#include <ao/rt/AppRuntime.h>

#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  namespace
  {
    /**
     * @brief library.listTree
     */
    class ListTreeComponent final : public ILayoutComponent
    {
    public:
      ListTreeComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
      {
        if (ctx.track.trackRowCache == nullptr)
        {
          _error = Gtk::make_managed<Gtk::Label>("Error: trackRowCache missing");
          return;
        }

        if (ctx.list.navigationController == nullptr)
        {
          _error = Gtk::make_managed<Gtk::Label>("Error: listNavigationController missing");
          return;
        }

        _controller = ctx.list.navigationController;

        // Initial rebuild
        auto const txn = ctx.runtime.musicLibrary().readTransaction();
        _controller->rebuildTree(*ctx.track.trackRowCache, txn);
      }

      Gtk::Widget& widget() override
      {
        return (_error != nullptr) ? static_cast<Gtk::Widget&>(*_error) : _controller->widget();
      }

    private:
      ListNavigationController* _controller = nullptr;
      Gtk::Label* _error = nullptr;
    };

    std::unique_ptr<ILayoutComponent> createListTree(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<ListTreeComponent>(ctx, node);
    }
  } // namespace

  void registerListTreeComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "library.listTree",
                                .displayName = "Library Tree",
                                .category = ComponentCategory::Library,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createListTree);
  }
} // namespace ao::gtk::layout
