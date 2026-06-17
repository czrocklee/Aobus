// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SemanticComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "track/TrackPageHost.h"
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/stack.h>
#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel::layout;
  namespace
  {
    /**
     * @brief tracks.table
     */
    class TracksTableComponent final : public ILayoutComponent
    {
    public:
      TracksTableComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
      {
        if (ctx.track.pageHost == nullptr)
        {
          _container.append(*Gtk::make_managed<Gtk::Label>("Error: trackPageHost missing"));
          return;
        }

        Gtk::Stack& stack = ctx.track.pageHost->stack();
        _container.append(stack);
        _container.set_hexpand(true);
        _container.set_vexpand(true);
      }

      Gtk::Widget& widget() override { return _container; }

    private:
      Gtk::Box _container{Gtk::Orientation::VERTICAL};
    };

    std::unique_ptr<ILayoutComponent> createTracksTable(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TracksTableComponent>(ctx, node);
    }
  } // namespace

  void registerTracksTableComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "tracks.table",
                                .displayName = "Tracks Table",
                                .category = ComponentCategory::Track,
                                .props = {{.name = "view",
                                           .kind = PropertyKind::String,
                                           .label = "View Source",
                                           .defaultValue = LayoutValue{"workspace.focused"}}},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createTracksTable);
  }
} // namespace ao::gtk::layout
